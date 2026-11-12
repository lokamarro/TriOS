// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The pieces of x86's SIMD floating-point arithmetic that the model computes
 * with SoftFloat and integers, because the host's differ: a lane's classes,
 * a result rounded as x86 rounds it, and a single rounded to a half. They
 * are apart from the model so that a test can hold the half rounding to a
 * second derivation. The functions have internal linkage, so that the model
 * inlines its own copy and each test includes one without a symbol. */

#include <SoftFloat-3e/platform.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::cpu::x86_64::sse_float {

/* An IEEE binary32 or binary64 lane's classes. */
struct Format final {
	bool dbl{};

	[[nodiscard]] std::uint64_t sign() const noexcept
	{
		return dbl ? 1ULL << 63U : 1ULL << 31U;
	}
	[[nodiscard]] std::uint64_t magnitude() const noexcept
	{
		return sign() - 1U;
	}
	[[nodiscard]] std::uint64_t exponent() const noexcept
	{
		return dbl ? 0x7ffULL << 52U : 0xffULL << 23U;
	}
	[[nodiscard]] std::uint64_t quiet_bit() const noexcept
	{
		return dbl ? 1ULL << 51U : 1ULL << 22U;
	}
	[[nodiscard]] std::uint64_t one() const noexcept
	{
		return dbl ? 0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL;
	}
	/* x86's default NaN, the indefinite. */
	[[nodiscard]] std::uint64_t indefinite() const noexcept
	{
		return sign() | exponent() | quiet_bit();
	}
	[[nodiscard]] bool nan(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) > exponent();
	}
	[[nodiscard]] bool signalling(std::uint64_t x) const noexcept
	{
		return nan(x) && (x & quiet_bit()) == 0;
	}
	[[nodiscard]] bool infinite(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) == exponent();
	}
	[[nodiscard]] bool zero(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) == 0;
	}
	[[nodiscard]] bool denormal(std::uint64_t x) const noexcept
	{
		return (x & exponent()) == 0 && !zero(x);
	}
	[[nodiscard]] bool negative(std::uint64_t x) const noexcept
	{
		return (x & sign()) != 0;
	}
	[[nodiscard]] std::uint64_t quiet(std::uint64_t x) const noexcept
	{
		return x | quiet_bit();
	}
	/* `x` with a denormal taken as a zero of its sign. */
	[[nodiscard]] std::uint64_t flush(std::uint64_t x) const noexcept
	{
		return denormal(x) ? x & sign() : x;
	}
	/* Ordered a < b, for numbers that are not both zeros. */
	[[nodiscard]] bool less(std::uint64_t a, std::uint64_t b) const noexcept
	{
		const auto key = [this](std::uint64_t x) {
			const auto value = static_cast<std::int64_t>(x & magnitude());
			return negative(x) ? -value : value;
		};
		return key(a) < key(b);
	}
};

/* Lane `index` of a vector of bytes. */
template <typename Vector>
[[nodiscard, maybe_unused]] static std::uint64_t lane(const Vector &vector, bool dbl,
	std::size_t index) noexcept
{
	if (dbl) {
		std::uint64_t value{};
		std::memcpy(&value, vector.data() + index * 8U, 8U);
		return value;
	}
	std::uint32_t value{};
	std::memcpy(&value, vector.data() + index * 4U, 4U);
	return value;
}

/* SoftFloat's rounding mode for MXCSR.RC. */
[[nodiscard, maybe_unused]] static std::uint8_t soft_rounding(std::uint32_t mxcsr) noexcept
{
	switch ((mxcsr >> 13U) & 3U) {
	case 1: return softfloat_round_min;
	case 2: return softfloat_round_max;
	case 3: return softfloat_round_minMag;
	default: return softfloat_round_near_even;
	}
}

/* A lane's value, exactly. */
[[nodiscard, maybe_unused]] static float128_t soft_wide(bool dbl, std::uint64_t x) noexcept
{
	softfloat_state state{};
	if (!dbl)
		return f32_to_f128(&state, float32_t{static_cast<std::uint32_t>(x)});
	return extF80_to_f128(&state, f64_to_extF80(&state, float64_t{x}));
}

[[nodiscard, maybe_unused]] static bool soft_zero(float128_t x) noexcept
{
	return x.v[0] == 0 && (x.v[1] << 1U) == 0;
}

/* `operation` rounded to odd: truncated, with its lowest bit set when that
 * lost anything. A single or double keeps at most 53 of binary128's 113
 * bits, so rounding it again rounds as the exact result would. An exact
 * zero takes its sign from the rounding mode, `mode`. SoftFloat's flags
 * for the operation land in `flags`. */
template <typename Operation>
[[nodiscard, maybe_unused]] static float128_t soft_odd(Operation operation,
	std::uint8_t mode, std::uint8_t &flags) noexcept
{
	softfloat_state state{};
	state.roundingMode = softfloat_round_minMag;
	auto result = operation(state);
	flags = state.exceptionFlags;
	if ((flags & softfloat_flag_inexact) != 0) {
		result.v[0] |= 1U;
	} else if (soft_zero(result)) {
		softfloat_state exact{};
		exact.roundingMode = mode;
		result = operation(exact);
	}
	return result;
}

/* A result rounded as x86 does: whether it is tiny after rounding, whether
 * it is inexact and whether it overflowed, and the result; and whether it
 * is inexact rounded to full precision as if the exponent were unbounded,
 * which is what an unmasked underflow or overflow reports as precision. */
struct Rounded final {
	std::uint64_t bits{};
	bool tiny{};
	bool inexact{};
	bool overflow{};
	bool significand_inexact{};
};

/* `value`, a binary128 that holds a result rounded to odd, rounded to a
 * single or a double in the rounding mode of `mxcsr`. */
[[nodiscard, maybe_unused]] static Rounded soft_round(float128_t value, bool dbl,
	std::uint32_t mxcsr) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	state.roundingMode = soft_rounding(mxcsr);
	Rounded result{};
	result.bits = dbl ? f128_to_f64(&state, value).v :
		f128_to_f32(&state, value).v;
	/* SoftFloat raises underflow for a tiny inexact result; an exact one
	 * is a denormal. */
	result.tiny = (state.exceptionFlags & softfloat_flag_underflow) != 0 ||
		Format{dbl}.denormal(result.bits);
	result.inexact = (state.exceptionFlags & softfloat_flag_inexact) != 0;
	result.overflow = (state.exceptionFlags & softfloat_flag_overflow) != 0;
	/* Of 112 fraction bits, a double keeps 52 and drops 60, and a single
	 * keeps 23 and drops 89. */
	result.significand_inexact = dbl ?
		(value.v[0] & ((1ULL << 60U) - 1U)) != 0 :
		value.v[0] != 0 || (value.v[1] & ((1ULL << 25U) - 1U)) != 0;
	return result;
}

/* Whether the finite lane `x` is an integer. */
[[nodiscard, maybe_unused]] static bool integral(const Format &f, std::uint64_t x) noexcept
{
	if (f.zero(x))
		return true;
	const unsigned fraction = f.dbl ? 52U : 23U;
	const unsigned bias = f.dbl ? 1023U : 127U;
	const auto exponent =
		static_cast<unsigned>((x & f.exponent()) >> fraction);
	if (exponent >= bias + fraction)
		return true;
	if (exponent < bias)
		return false;
	return (x & ((1ULL << (bias + fraction - exponent)) - 1U)) == 0;
}

/* The single `x`, neither zero, infinite nor a NaN, rounded to a half under
 * MXCSR.RC-style `control`, in integers: `x` is m * 2^shift with an integer
 * m of up to 24 bits. Halves have 11-bit significands down to 2^-14 and are
 * multiples of 2^-24 below it; the largest is 65504. A result is tiny when,
 * rounded to 11 bits as if the exponent were unbounded, it lies below
 * 2^-14: rounding to nearest reaches 2^-14 from 2^-14 - 2^-26 up, rounding
 * away from zero from above 2^-14 - 2^-25, and rounding toward zero never.
 * Rounding to nearest overflows from 65520, away from zero above 65504, and
 * toward zero from 2^16. */
[[nodiscard, maybe_unused]] static Rounded half_round(std::uint32_t x,
	unsigned control) noexcept
{
	const bool negative = (x >> 31U) != 0;
	const unsigned exponent = (x >> 23U) & 0xffU;
	const std::uint64_t m = exponent == 0 ? x & 0x7fffffU :
		(x & 0x7fffffU) | 0x800000U;
	const int shift = exponent == 0 ? -149 : static_cast<int>(exponent) - 150;
	/* Rounding to nearest, away from zero, or toward zero. */
	const unsigned direction = (control & 3U) == 0 ? 0U :
		(control & 3U) == 3U ? 2U :
		((control & 3U) == 1U) == negative ? 1U : 2U;
	/* m * 2^shift against k * 2^power, for a k below 2^12: -1, 0 or 1.
	 * Scaled 2^40 apart, the larger side wins whatever m and k are. */
	const auto versus = [&](std::uint64_t k, int power) {
		std::uint64_t left = m;
		std::uint64_t right = k;
		if (shift >= power) {
			if (shift - power >= 40)
				return 1;
			left <<= static_cast<unsigned>(shift - power);
		} else {
			if (power - shift >= 40)
				return -1;
			right <<= static_cast<unsigned>(power - shift);
		}
		return left < right ? -1 : left > right ? 1 : 0;
	};
	const int top = 63 - __builtin_clzll(m);
	Rounded result{};
	/* An unmasked tiny half of a denormal single raises precision too, as
	 * an Intel Xeon 8581C does. */
	result.significand_inexact = exponent == 0 || (top > 10 &&
		(m & ((1ULL << static_cast<unsigned>(top - 10)) - 1U)) != 0);
	if (top + shift < -14) {
		result.tiny = direction == 2 ||
			(direction == 0 ? versus(4095, -26) < 0 : versus(2047, -25) <= 0);
		/* Halves step by 2^-24 there; a value below the least half has
		 * every bit below that step. */
		const int below = -(shift + 24);
		result.inexact = below > top ||
			(below > 0 &&
				(m & ((1ULL << static_cast<unsigned>(below)) - 1U)) != 0);
		return result;
	}
	if (top + shift >= 16) {
		result.overflow = true;
	} else if (top + shift == 15) {
		result.overflow = direction == 0 ? versus(4095, 4) >= 0 :
			direction == 1 ? versus(2047, 5) > 0 : false;
	}
	result.inexact = result.overflow || result.significand_inexact;
	return result;
}

} // namespace uml::cpu::x86_64::sse_float
