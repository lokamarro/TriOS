// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/floating_integer_conversion.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>

/*
 * The precision conversions (FCVT, FCVTXN) and the integer conversions
 * (SCVTF, UCVTF) round and pack through one shared tail. This test pins its
 * results and FPSR flags for every rounding mode, overflow, subnormal
 * results, the FZ and FZ16 flush controls, FPCR.AHP and round to odd. The
 * expected values are what an Apple core produces for the same instruction
 * under the same FPCR; the engines' own behaviour is covered by the corpus.
 */

namespace detail = uml::cpu::arm64::detail;

namespace {

constexpr unsigned none = ~0U;

/* A precision conversion: the widths, the source encoding, FPCR, the
 * rounding override (none for FPCR's mode), then the result and FPSR flags. */
struct PrecisionCase final {
	unsigned source_width;
	unsigned destination_width;
	std::uint64_t input;
	std::uint32_t fpcr;
	unsigned rounding;
	std::uint64_t bits;
	std::uint32_t exceptions;
};

/* An integer conversion: the widths, the fractional bits, signedness, the
 * source integer and FPCR, then the result and FPSR flags. */
struct IntegerCase final {
	unsigned integer_width;
	unsigned destination_width;
	unsigned fractional_bits;
	bool unsigned_source;
	std::uint64_t input;
	std::uint32_t fpcr;
	std::uint64_t bits;
	std::uint32_t exceptions;
};

constexpr PrecisionCase precision_cases[] = {
	/* 1 + 2^-12 into half, every rounding mode. */
	{64, 16, 0x3ff0010000000000, 0x0, none, 0x3c00, 0x10},
	{64, 16, 0x3ff0010000000000, 0x400000, none, 0x3c01, 0x10},
	{64, 16, 0x3ff0010000000000, 0x800000, none, 0x3c00, 0x10},
	{64, 16, 0x3ff0010000000000, 0xc00000, none, 0x3c00, 0x10},
	/* 65520.0 into half: only the nearest and plus-infinity modes
	 * overflow. */
	{32, 16, 0x477ff000, 0x0, none, 0x7c00, 0x14},
	{32, 16, 0x477ff000, 0x400000, none, 0x7c00, 0x14},
	{32, 16, 0x477ff000, 0x800000, none, 0x7bff, 0x10},
	{32, 16, 0x477ff000, 0xc00000, none, 0x7bff, 0x10},
	/* The alternative half format has no infinity, so 65520.0 is an
	 * ordinary value there. */
	{32, 16, 0x477ff000, 0x4000000, none, 0x7c00, 0x10},
	{32, 16, 0xc77ff000, 0x4c00000, none, 0xfbff, 0x10},
	/* 2^-20 into half is subnormal. A precision conversion ignores both
	 * FZ and FZ16 for a half result. */
	{64, 16, 0x3eb0000000000000, 0x0, none, 0x10, 0x0},
	{64, 16, 0x3eb0000040000000, 0x0, none, 0x10, 0x18},
	{64, 16, 0x3eb0000000000000, 0x1000000, none, 0x10, 0x0},
	{64, 16, 0x3eb0000040000000, 0x1000000, none, 0x10, 0x18},
	{64, 16, 0x3eb0000000000000, 0x80000, none, 0x10, 0x0},
	{64, 16, 0x3eb0000040000000, 0x80000, none, 0x10, 0x18},
	{64, 16, 0x3eb0000000000000, 0x1080000, none, 0x10, 0x0},
	{64, 16, 0x3eb0000040000000, 0x1080000, none, 0x10, 0x18},
	/* 2^-140 into single is subnormal. FZ flushes it, raising only
	 * Underflow; FZ16 does not. */
	{64, 32, 0x3730000000000000, 0x0, none, 0x200, 0x0},
	{64, 32, 0x3730000040000000, 0x0, none, 0x200, 0x18},
	{64, 32, 0x3730000000000000, 0x1000000, none, 0x0, 0x8},
	{64, 32, 0x3730000040000000, 0x1000000, none, 0x0, 0x8},
	{64, 32, 0x3730000000000000, 0x80000, none, 0x200, 0x0},
	{64, 32, 0x3730000040000000, 0x80000, none, 0x200, 0x18},
	/* A negative inexact subnormal result, every rounding mode. */
	{64, 32, 0xb730000040000000, 0x0, none, 0x80000200, 0x18},
	{64, 32, 0xb730000040000000, 0x400000, none, 0x80000200, 0x18},
	{64, 32, 0xb730000040000000, 0x800000, none, 0x80000201, 0x18},
	{64, 32, 0xb730000040000000, 0xc00000, none, 0x80000200, 0x18},
	/* Double overflow into single, every rounding mode. */
	{64, 32, 0x47f0000000000000, 0x0, none, 0x7f800000, 0x14},
	{64, 32, 0x47f0000000000000, 0x400000, none, 0x7f800000, 0x14},
	{64, 32, 0x47f0000000000000, 0x800000, none, 0x7f7fffff, 0x14},
	{64, 32, 0x47f0000000000000, 0xc00000, none, 0x7f7fffff, 0x14},
	/* Round to odd, as FCVTXN rounds. */
	{64, 32, 0x3ff0000000400000, 0x0, 4, 0x3f800001, 0x10},
	{64, 32, 0x47f0000000000000, 0x0, 4, 0x7f7fffff, 0x14},
	/* The alternative half format saturates, and has no infinity or NaN. */
	{32, 16, 0x48000000, 0x4000000, none, 0x7fff, 0x1},
	{64, 16, 0x7ff0000000000000, 0x4000000, none, 0x7fff, 0x1},
	{64, 16, 0x7ff8000000000000, 0x4000000, none, 0x0, 0x1},
	/* IEEE half: a signalling NaN, a quiet NaN under DN and an infinity. */
	{64, 16, 0x7ff0000000000001, 0x0, none, 0x7e00, 0x1},
	{64, 16, 0x7ff8000000000001, 0x2000000, none, 0x7e00, 0x0},
	{64, 16, 0xfff0000000000000, 0x0, none, 0xfc00, 0x0},
	/* Widening is exact. */
	{16, 32, 0x1, 0x0, none, 0x33800000, 0x0},
	{16, 64, 0x3ff, 0x0, none, 0x3f0ff80000000000, 0x0},
	{32, 64, 0x3fc00000, 0x0, none, 0x3ff8000000000000, 0x0},
};

constexpr IntegerCase integer_cases[] = {
	/* 16777217 into single, every rounding mode. */
	{32, 32, 0, false, 0x1000001, 0x0, 0x4b800000, 0x10},
	{32, 32, 0, false, 0x1000001, 0x400000, 0x4b800001, 0x10},
	{32, 32, 0, false, 0x1000001, 0x800000, 0x4b800000, 0x10},
	{32, 32, 0, false, 0x1000001, 0xc00000, 0x4b800000, 0x10},
	/* Overflow into half, signed and unsigned, nearest and zero. */
	{32, 16, 0, true, 0xffffffff, 0x0, 0x7c00, 0x14},
	{32, 16, 0, true, 0xffffffff, 0xc00000, 0x7bff, 0x14},
	{32, 16, 0, false, 0x80000000, 0x0, 0xfc00, 0x14},
	/* 2^-16 into half is subnormal. FZ16 flushes it; FZ does not. */
	{32, 16, 16, false, 0x1, 0x0, 0x100, 0x0},
	{32, 16, 16, false, 0x1, 0x1000000, 0x100, 0x0},
	{32, 16, 16, false, 0x1, 0x80000, 0x0, 0x8},
	/* 3 * 2^-25 into half is a tie in the subnormal range. */
	{64, 16, 25, false, 0x3, 0x0, 0x2, 0x18},
	{64, 16, 25, false, 0x3, 0x400000, 0x2, 0x18},
	{64, 16, 25, false, 0x3, 0x800000, 0x1, 0x18},
	{64, 16, 25, false, 0x3, 0xc00000, 0x1, 0x18},
	{64, 16, 25, false, 0x3, 0x80000, 0x0, 0x8},
	{64, 16, 25, false, 0xfffffffffffffffd, 0x0, 0x8002, 0x18},
	/* 2^-64 into single is normal, and (2^40 - 1) / 2^40 rounds up to
	 * one. */
	{64, 32, 64, false, 0x1, 0x0, 0x1f800000, 0x0},
	{64, 16, 40, true, 0xffffffffff, 0x0, 0x3c00, 0x10},
	/* Wide integers into double. */
	{64, 64, 0, false, 0xffffffffffffffff, 0x0, 0xbff0000000000000, 0x0},
	{64, 64, 0, false, 0x7fffffffffffffff, 0x0, 0x43e0000000000000, 0x10},
	{64, 64, 0, false, 0x7fffffffffffffff, 0xc00000,
		0x43dfffffffffffff, 0x10},
	{32, 64, 8, true, 0xffffffff, 0x0, 0x416fffffffe00000, 0x0},
};

[[nodiscard]] constexpr bool precision_holds(const PrecisionCase &c)
{
	const auto result = detail::convert_floating_precision(c.input,
		c.source_width, c.destination_width, c.fpcr, c.rounding);
	return result.bits == c.bits && result.exceptions == c.exceptions;
}

[[nodiscard]] constexpr bool integer_holds(const IntegerCase &c)
{
	const auto result = detail::convert_integer_to_floating_bits(c.input,
		c.integer_width, c.destination_width, c.fractional_bits,
		c.unsigned_source, c.fpcr);
	return result.bits == c.bits && result.exceptions == c.exceptions;
}

/* The 1-based position of the first case that fails, or zero. */
[[nodiscard]] constexpr std::size_t first_precision_failure()
{
	for (std::size_t i = 0; i < std::size(precision_cases); ++i)
		if (!precision_holds(precision_cases[i]))
			return i + 1;
	return 0;
}

[[nodiscard]] constexpr std::size_t first_integer_failure()
{
	for (std::size_t i = 0; i < std::size(integer_cases); ++i)
		if (!integer_holds(integer_cases[i]))
			return i + 1;
	return 0;
}

static_assert(first_precision_failure() == 0);
static_assert(first_integer_failure() == 0);

} // namespace

/* The same cases at run time, with inputs the compiler cannot fold. */
int main()
{
	int failures = 0;
	for (auto c : precision_cases) {
		const volatile std::uint64_t input = c.input;
		c.input = input;
		if (!precision_holds(c)) {
			std::printf("conversion: precision %llx fails\n",
				static_cast<unsigned long long>(c.input));
			++failures;
		}
	}
	for (auto c : integer_cases) {
		const volatile std::uint64_t input = c.input;
		c.input = input;
		if (!integer_holds(c)) {
			std::printf("conversion: integer %llx fails\n",
				static_cast<unsigned long long>(c.input));
			++failures;
		}
	}
	if (failures == 0)
		std::puts("arm64-floating-conversion: conversion tail holds");
	return failures == 0 ? 0 : 1;
}
