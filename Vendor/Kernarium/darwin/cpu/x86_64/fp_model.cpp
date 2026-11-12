// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/x86_64/fp_model.hpp"

#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/sse_float.hpp"

#include <SoftFloat-3e/platform.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace uml::cpu::x86_64 {
namespace {

using namespace sse_float;

constexpr std::uint32_t invalid = 0x01U;
constexpr std::uint32_t denormal = 0x02U;
constexpr std::uint32_t divide = 0x04U;
constexpr std::uint32_t overflow = 0x08U;
constexpr std::uint32_t underflow = 0x10U;
constexpr std::uint32_t precision = 0x20U;
constexpr std::uint32_t mask_shift = 7U;

void set_lane(FpVector &vector, bool dbl, std::size_t index,
	std::uint64_t value) noexcept
{
	if (dbl) {
		std::memcpy(vector.data() + index * 8U, &value, 8U);
		return;
	}
	const auto narrow = static_cast<std::uint32_t>(value);
	std::memcpy(vector.data() + index * 4U, &narrow, 4U);
}

/* One lane's result and the flags it raises before and after computing. */
struct Lane final {
	std::uint64_t bits{};
	std::uint32_t before{};
	std::uint32_t after{};
};

/* A rounded result's lane. A tiny result raises underflow when it is
 * inexact, or regardless while MXCSR unmasks underflow. While it masks
 * underflow, FTZ flushes a tiny result to zero, which raises underflow and
 * precision even when it was exact. Beside an unmasked underflow or
 * overflow, which faults, precision follows the significand alone. */
[[nodiscard]] Lane finish(const Format &f, const Rounded &rounded,
	std::uint32_t mxcsr, std::uint32_t before) noexcept
{
	const bool underflow_unmasked = (mxcsr & (underflow << mask_shift)) == 0;
	const bool overflow_unmasked = (mxcsr & (overflow << mask_shift)) == 0;
	Lane result{rounded.bits, before, 0};
	bool inexact = rounded.inexact;
	if (rounded.tiny && (mxcsr & mxcsr_flush_to_zero) != 0 &&
		!underflow_unmasked) {
		result.bits &= f.sign();
		result.after |= underflow;
		inexact = true;
	} else if (rounded.tiny && underflow_unmasked) {
		result.after |= underflow;
		inexact = rounded.significand_inexact;
	} else if (rounded.tiny && rounded.inexact) {
		result.after |= underflow;
	} else if (rounded.overflow) {
		result.after |= overflow;
		if (overflow_unmasked)
			inexact = rounded.significand_inexact;
	}
	if (inexact)
		result.after |= precision;
	return result;
}

/* ADD, SUB, MUL and DIV. A NaN result is the first source's when it is a
 * NaN, quieted, and else the second's; an invalid operation gives the
 * indefinite. Invalid operation and division by zero take precedence over a
 * denormal operand. */
template <bool Dbl>
[[nodiscard]] Lane binary(FpKind kind, std::uint64_t a, std::uint64_t b,
	std::uint32_t mxcsr) noexcept
{
	constexpr Format f{Dbl};
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	Lane result{};
	if (f.signalling(a) || f.signalling(b))
		result.before |= invalid;
	if (f.nan(a) || f.nan(b)) {
		result.bits = f.nan(a) ? f.quiet(a) : f.quiet(b);
		return result;
	}
	const auto x = soft_wide(f.dbl, a);
	const auto y = soft_wide(f.dbl, b);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		switch (kind) {
		case FpKind::add: return f128_add(&state, x, y);
		case FpKind::sub: return f128_sub(&state, x, y);
		case FpKind::mul: return f128_mul(&state, x, y);
		default: return f128_div(&state, x, y);
		}
	}, soft_rounding(mxcsr), flags);
	if ((flags & softfloat_flag_invalid) != 0) {
		result.bits = f.indefinite();
		result.before |= invalid;
		return result;
	}
	if ((flags & softfloat_flag_infinite) != 0) {
		result.bits = ((a ^ b) & f.sign()) | f.exponent();
		result.before |= divide;
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b)))
		result.before |= denormal;
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

[[nodiscard]] Lane binary(const Format &f, FpKind kind, std::uint64_t a,
	std::uint64_t b, std::uint32_t mxcsr) noexcept
{
	return f.dbl ? binary<true>(kind, a, b, mxcsr) :
		binary<false>(kind, a, b, mxcsr);
}

/* The residues a square leaves modulo `modulus`, as the bits of a mask. */
template <unsigned Modulus>
[[nodiscard]] constexpr std::uint64_t square_residues() noexcept
{
	std::uint64_t mask = 0;
	for (unsigned residue = 0; residue < Modulus; ++residue)
		mask |= 1ULL << (residue * residue % Modulus);
	return mask;
}

/* Whether the square root of the positive normal number `x` is exact: its
 * odd significand is a perfect square and its exponent is even, because the
 * root of anything else is irrational. An odd square is 1 modulo 8, which
 * with the exponent's parity rules out seven lanes in eight; residues
 * modulo 63 and 11 rule out most of the rest, and a bitwise root settles
 * them. */
[[nodiscard]] bool exact_square_root(const Format &f, std::uint64_t x) noexcept
{
	constexpr auto mod63 = square_residues<63>();
	constexpr auto mod11 = square_residues<11>();
	const unsigned fraction = f.dbl ? 52U : 23U;
	const unsigned bias = f.dbl ? 1023U : 127U;
	const auto significand = (x & ((1ULL << fraction) - 1U)) | (1ULL << fraction);
	const auto trailing = static_cast<unsigned>(__builtin_ctzll(significand));
	const int exponent = static_cast<int>((x & f.exponent()) >> fraction) -
		static_cast<int>(bias + fraction) + static_cast<int>(trailing);
	const auto m = significand >> trailing;
	if ((((m & 7U) ^ 1U) | (static_cast<std::uint64_t>(exponent) & 1U)) != 0 ||
		((mod63 >> (m % 63U)) & (mod11 >> (m % 11U)) & 1U) == 0)
		return false;
	std::uint64_t root = 0;
	for (std::uint64_t bit = 1ULL << ((63U - __builtin_clzll(m)) / 2U);
		bit != 0; bit >>= 1U) {
		const auto trial = root | bit;
		if (trial * trial <= m)
			root = trial;
	}
	return root * root == m;
}

/* SQRT: the square root of a negative number but -0 is invalid. */
[[nodiscard]] Lane square_root(const Format &f, std::uint64_t b,
	std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz)
		b = f.flush(b);
	Lane result{};
	if (f.nan(b)) {
		result.bits = f.quiet(b);
		result.before = f.signalling(b) ? invalid : 0U;
		return result;
	}
	if (f.negative(b) && !f.zero(b)) {
		result.bits = f.indefinite();
		result.before = invalid;
		return result;
	}
	if (!daz && f.denormal(b))
		result.before |= denormal;
	const auto x = soft_wide(f.dbl, b);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		return f128_sqrt(&state, x);
	}, soft_rounding(mxcsr), flags);
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

/* MIN and MAX return the second source for a NaN, quiet or not, which is
 * invalid, and for two zeros. They round nothing, so FTZ leaves a denormal
 * result. */
[[nodiscard]] Lane minimum_maximum(const Format &f, bool maximum,
	std::uint64_t a, std::uint64_t b, std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	Lane result{b, 0, 0};
	if (f.nan(a) || f.nan(b)) {
		result.before = invalid;
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b)))
		result.before = denormal;
	/* Two zeros are equal, whatever their signs. A mask picks the result,
	 * because lanes that compare either way make a branch unpredictable. */
	const auto left = maximum ? b : a;
	const auto right = maximum ? a : b;
	const auto pick_first = 0 - static_cast<std::uint64_t>(f.less(left, right));
	result.bits = b ^ ((a ^ b) & pick_first);
	return result;
}

/* An FMA lane, a * b + c with the product and the addend negated as asked.
 * A NaN result is the first of a, b and c that is a NaN, quieted and with
 * its own sign. Invalid operation is a signalling NaN, infinity times zero
 * plus anything but a quiet NaN, or infinities of opposite signs added. */
[[nodiscard]] Lane fused(const Format &f, std::uint64_t a, std::uint64_t b,
	std::uint64_t c, bool negate_product, bool negate_addend,
	std::uint32_t mxcsr) noexcept
{
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
		c = f.flush(c);
	}
	const bool product_negative = f.negative(a ^ b) != negate_product;
	const bool addend_negative = f.negative(c) != negate_addend;
	bool invalid_operation = false;
	if (f.signalling(a) || f.signalling(b) || f.signalling(c))
		invalid_operation = true;
	else if ((f.zero(a) && f.infinite(b)) || (f.infinite(a) && f.zero(b)))
		invalid_operation = !f.nan(c);
	else if (!f.nan(a) && !f.nan(b) && !f.nan(c))
		invalid_operation = (f.infinite(a) || f.infinite(b)) &&
			f.infinite(c) && product_negative != addend_negative;
	Lane result{};
	if (invalid_operation)
		result.before = invalid;
	for (const auto value : {a, b, c}) {
		if (f.nan(value)) {
			result.bits = f.quiet(value);
			return result;
		}
	}
	if (invalid_operation) {
		result.bits = f.indefinite();
		return result;
	}
	if (!daz && (f.denormal(a) || f.denormal(b) || f.denormal(c)))
		result.before |= denormal;
	const auto x = soft_wide(f.dbl, negate_product ? a ^ f.sign() : a);
	const auto y = soft_wide(f.dbl, b);
	const auto z = soft_wide(f.dbl, negate_addend ? c ^ f.sign() : c);
	/* Exact: a double's product has 106 bits. */
	softfloat_state exact{};
	const auto product = f128_mul(&exact, x, y);
	std::uint8_t flags{};
	const auto value = soft_odd([&](softfloat_state &state) {
		return f128_add(&state, product, z);
	}, soft_rounding(mxcsr), flags);
	return finish(f, soft_round(value, f.dbl, mxcsr), mxcsr, result.before);
}

/* CVTSD2SS and CVTPD2PS. A NaN keeps its sign and the top of its payload,
 * quieted. */
[[nodiscard]] Lane narrow(std::uint64_t x, std::uint32_t mxcsr) noexcept
{
	const Format wide{true};
	const Format single{false};
	const bool daz = (mxcsr & mxcsr_denormals_are_zero) != 0;
	if (daz)
		x = wide.flush(x);
	Lane result{};
	if (wide.nan(x)) {
		result.bits = ((x >> 32U) & 0x8000'0000U) | 0x7fc0'0000U |
			((x >> 29U) & 0x003f'ffffU);
		result.before = wide.signalling(x) ? invalid : 0U;
		return result;
	}
	if (!daz && wide.denormal(x))
		result.before = denormal;
	return finish(single, soft_round(soft_wide(true, x), false, mxcsr), mxcsr,
		result.before);
}

/* RCPPS, RCPSS, RSQRTPS and RSQRTSS, which ignore MXCSR but its rounding and
 * raise nothing: a denormal input is a zero, a reciprocal of 2^126 or more is
 * a zero of the input's sign, and the reciprocal square root of a negative
 * number is the indefinite. Elsewhere the correctly rounded quotient, of the
 * rounded square root for RSQRT, lies within x86's error, and is what the
 * host computes for the lanes that need nothing more. */
[[nodiscard]] std::uint64_t estimate(bool reciprocal, std::uint64_t x,
	std::uint32_t mxcsr) noexcept
{
	const Format f{false};
	x = f.flush(x);
	if (f.nan(x))
		return f.quiet(x);
	const auto magnitude = x & f.magnitude();
	const auto one = soft_wide(false, f.one());
	const auto mode = soft_rounding(mxcsr);
	std::uint8_t flags{};
	if (reciprocal) {
		if (magnitude >= 0x7e80'0000U && magnitude <= 0x7f80'0000U)
			return x & f.sign();
		const auto wide = soft_wide(false, x);
		return soft_round(soft_odd([&](softfloat_state &state) {
			return f128_div(&state, one, wide);
		}, mode, flags), false, mxcsr).bits;
	}
	if (f.negative(x) && magnitude != 0)
		return f.indefinite();
	const auto wide = soft_wide(false, x);
	const auto root = soft_round(soft_odd([&](softfloat_state &state) {
		return f128_sqrt(&state, wide);
	}, mode, flags), false, mxcsr).bits;
	const auto wide_root = soft_wide(false, root);
	return soft_round(soft_odd([&](softfloat_state &state) {
		return f128_div(&state, one, wide_root);
	}, mode, flags), false, mxcsr).bits;
}

/* DPPS or DPPD on one 128-bit part: the selected products, zero
 * elsewhere, then their sums, with x86's rules at every step. Each result
 * lane sums in its own order, which picks among NaNs as x86 hardware does:
 * for DPPS lane i adds p[i ^ 1] + p[i] and then that sum and the other
 * pair's, for DPPD p[i] + p[i ^ 1]. The steps raise their flags in turn:
 * the products, the sums of pairs, and for DPPS their sum. An unselected
 * product is zero and raises nothing. */
[[nodiscard]] FpSteps dot_product(const Format &f, std::uint8_t control,
	const FpValues &values, FpVector &result, std::uint32_t mxcsr) noexcept
{
	FpSteps steps{};
	const std::size_t count = f.dbl ? 2U : 4U;
	std::array<std::uint64_t, 4> products{};
	for (std::size_t index = 0; index < count; ++index) {
		if (((control >> (4U + index)) & 1U) == 0)
			continue;
		const auto product = binary(f, FpKind::mul,
			lane(values.sources[0], f.dbl, index),
			lane(values.sources[1], f.dbl, index), mxcsr);
		products[index] = product.bits;
		steps.before[0] |= product.before;
		steps.after[0] |= product.after;
	}
	const auto add = [&](std::uint64_t a, std::uint64_t b, std::size_t step) {
		const auto sum = binary(f, FpKind::add, a, b, mxcsr);
		steps.before[step] |= sum.before;
		steps.after[step] |= sum.after;
		return sum.bits;
	};
	/* The sums of a and b in either order raise the same flags, and
	 * give the same bits unless a NaN picks the order. */
	const auto either = [&](std::uint64_t a, std::uint64_t b,
		std::size_t step, std::uint64_t &first, std::uint64_t &second) {
		first = add(a, b, step);
		second = f.nan(a) || f.nan(b) ? add(b, a, step) : first;
	};
	std::array<std::uint64_t, 4> sums{};
	if (f.dbl) {
		either(products[0], products[1], 1, sums[0], sums[1]);
	} else {
		std::array<std::uint64_t, 4> pairs{};
		either(products[1], products[0], 1, pairs[0], pairs[1]);
		either(products[3], products[2], 1, pairs[2], pairs[3]);
		if (f.nan(pairs[0]) || f.nan(pairs[1]) || f.nan(pairs[2]) ||
			f.nan(pairs[3])) {
			for (std::size_t index = 0; index < 4; ++index)
				sums[index] = add(pairs[index], pairs[index ^ 2U], 2);
		} else {
			sums.fill(add(pairs[0], pairs[2], 2));
		}
	}
	for (std::size_t index = 0; index < count; ++index)
		set_lane(result, f.dbl, index,
			((control >> index) & 1U) != 0 ? sums[index] : 0U);
	return steps;
}

/* A lane of `bytes` of the sources an exception-only kind reads: the first
 * source's 16 bytes, then the second's. */
[[nodiscard]] std::uint64_t packed_lane(const FpValues &values,
	std::size_t bytes, std::size_t index) noexcept
{
	const auto offset = index * bytes;
	const auto *source = values.sources[offset < 16U ? 0U : 1U].data() +
		offset % 16U;
	/* Constant sizes load in line; memcpy of a runtime size calls memmove. */
	switch (bytes) {
	case 2: {
		std::uint16_t value{};
		std::memcpy(&value, source, 2);
		return value;
	}
	case 4: {
		std::uint32_t value{};
		std::memcpy(&value, source, 4);
		return value;
	}
	default: {
		std::uint64_t value{};
		std::memcpy(&value, source, 8);
		return value;
	}
	}
}

/* Whether a comparison predicate signals for a quiet NaN: the legacy
 * encoding's three bits, or VEX's five, whose bit 4 makes a quiet predicate
 * of the low four bits signalling and the reverse. */
[[nodiscard]] bool signalling_predicate(std::uint8_t predicate,
	bool vex) noexcept
{
	constexpr std::uint32_t signalling_predicates = 0x6666U;
	const unsigned value = vex ? predicate & 0x1fU : predicate & 7U;
	return (((signalling_predicates >> (value & 0xfU)) & 1U) != 0) !=
		((value & 0x10U) != 0);
}

/* A comparison raises invalid operation for a signalling NaN, and for a
 * quiet one when it signals; without a NaN, a denormal operand raises its
 * flag. */
[[nodiscard]] std::uint32_t compare(const Format &f, bool signalling,
	std::uint64_t a, std::uint64_t b, std::uint32_t mxcsr) noexcept
{
	if (f.signalling(a) || f.signalling(b) ||
		(signalling && (f.nan(a) || f.nan(b))))
		return invalid;
	if (f.nan(a) || f.nan(b) || (mxcsr & mxcsr_denormals_are_zero) != 0)
		return 0;
	return f.denormal(a) || f.denormal(b) ? denormal : 0U;
}

/* A conversion to an integer: invalid operation for a NaN, an infinity or a
 * value out of range, which gives the integer indefinite, and else
 * precision for a value that is not an integer. */
[[nodiscard]] Lane to_integer(const Format &f, std::uint64_t x, bool truncate,
	bool wide, std::uint32_t mxcsr) noexcept
{
	if ((mxcsr & mxcsr_denormals_are_zero) != 0)
		x = f.flush(x);
	Lane result{};
	if (f.nan(x) || f.infinite(x)) {
		result.before = invalid;
		return result;
	}
	softfloat_state state{};
	const auto mode = truncate ? std::uint8_t{softfloat_round_minMag} :
		soft_rounding(mxcsr);
	const auto value = soft_wide(f.dbl, x);
	if (wide)
		(void)f128_to_i64(&state, value, mode, true);
	else
		(void)f128_to_i32(&state, value, mode, true);
	if ((state.exceptionFlags & softfloat_flag_invalid) != 0)
		result.before = invalid;
	else if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		result.after = precision;
	return result;
}

/* A conversion of an integer to a float, which raises precision when the
 * float cannot hold it: when the integer spans more bits than its
 * significand, whatever the rounding. */
[[nodiscard]] std::uint32_t from_integer(bool dbl, std::uint64_t x,
	bool wide) noexcept
{
	const auto integer = wide ? static_cast<std::int64_t>(x) :
		static_cast<std::int64_t>(static_cast<std::int32_t>(x));
	if (integer == 0)
		return 0;
	const auto magnitude = integer < 0 ?
		0 - static_cast<std::uint64_t>(integer) :
		static_cast<std::uint64_t>(integer);
	const unsigned span = 64U - static_cast<unsigned>(__builtin_clzll(magnitude)) -
		static_cast<unsigned>(__builtin_ctzll(magnitude));
	return span > (dbl ? 53U : 24U) ? precision : 0U;
}

/* ROUNDPS to ROUNDSD: invalid operation for a signalling NaN, and precision
 * for a lane that is not an integer unless bit 3 of the immediate suppresses
 * it, whatever the rounding. */
[[nodiscard]] Lane round_integral(const Format &f, std::uint64_t x,
	std::uint8_t control, std::uint32_t mxcsr) noexcept
{
	if ((mxcsr & mxcsr_denormals_are_zero) != 0)
		x = f.flush(x);
	Lane result{};
	if (f.nan(x)) {
		result.before = f.signalling(x) ? invalid : 0U;
		return result;
	}
	if ((control & 8U) == 0 && !f.infinite(x) && !integral(f, x))
		result.after = precision;
	return result;
}

/* VCVTPS2PH: invalid operation for a signalling NaN, a denormal operand
 * unless DAZ makes it a zero, then underflow, overflow and precision as the
 * half rounds, in the rounding its immediate names unless bit 2 defers to
 * MXCSR's. FTZ does not apply. */
[[nodiscard]] Lane half_narrowing(std::uint64_t x, std::uint8_t control,
	std::uint32_t mxcsr) noexcept
{
	const Format f{false};
	if ((mxcsr & mxcsr_denormals_are_zero) != 0)
		x = f.flush(x);
	Lane result{};
	if (f.nan(x)) {
		result.before = f.signalling(x) ? invalid : 0U;
		return result;
	}
	if (f.zero(x) || f.infinite(x))
		return result;
	if (f.denormal(x))
		result.before = denormal;
	const auto rounded = half_round(static_cast<std::uint32_t>(x),
		(control & 4U) != 0 ? (mxcsr >> 13U) & 3U : control & 3U);
	return finish(f, rounded, mxcsr & ~mxcsr_flush_to_zero, result.before);
}

} // namespace

FpSteps &FpSteps::operator|=(const FpSteps &other) noexcept
{
	for (std::size_t step = 0; step < count; ++step) {
		before[step] |= other.before[step];
		after[step] |= other.after[step];
	}
	return *this;
}

FpSteps x86_fp_compute(const FpOperation &operation, std::uint32_t mxcsr,
	FpValues &values) noexcept
{
	const Format f{operation.element_bytes == 8};
	/* A conversion narrows up to four doubles from two sources, and an
	 * exception-only kind reads up to 32 bytes. */
	const auto lanes = std::min<std::size_t>(operation.lanes,
		operation.kind == FpKind::narrow ? 4U :
		operation.kind >= FpKind::cmp ? 32U / operation.element_bytes :
		16U / operation.element_bytes);
	const auto &first = values.sources[0];
	const auto &second = values.sources[1];
	FpSteps steps{};
	const auto record = [&](std::size_t index, const Lane &result) {
		set_lane(values.result, f.dbl, index, result.bits);
		steps.before[0] |= result.before;
		steps.after[0] |= result.after;
	};
	switch (operation.kind) {
	case FpKind::add:
	case FpKind::sub:
	case FpKind::mul:
	case FpKind::div:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, binary(f, operation.kind, lane(first, f.dbl, index),
				lane(second, f.dbl, index), mxcsr));
		break;
	case FpKind::min:
	case FpKind::max:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, minimum_maximum(f, operation.kind == FpKind::max,
				lane(first, f.dbl, index), lane(second, f.dbl, index),
				mxcsr));
		break;
	case FpKind::sqrt:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto x = lane(first, f.dbl, index);
			/* The host's root of a positive normal number is x86's, and
			 * raises precision unless it is exact. */
			if ((x & f.sign()) == 0 && (x & f.exponent()) != 0 &&
				(x & f.exponent()) != f.exponent()) {
				if (!exact_square_root(f, x))
					steps.after[0] |= precision;
				continue;
			}
			record(index, square_root(f, x, mxcsr));
		}
		break;
	case FpKind::rcp:
	case FpKind::rsqrt:
		for (std::size_t index = 0; index < lanes; ++index)
			set_lane(values.result, f.dbl, index,
				estimate(operation.kind == FpKind::rcp,
					lane(first, f.dbl, index), mxcsr));
		break;
	case FpKind::hadd:
	case FpKind::hsub: {
		/* Pairs of the first source, then of the second, the lower
		 * element first. */
		const auto kind = operation.kind == FpKind::hadd ? FpKind::add :
			FpKind::sub;
		const std::size_t half = lanes / 2U;
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto &source = index < half ? first : second;
			const std::size_t pair = 2U * (index % half);
			record(index, binary(f, kind, lane(source, f.dbl, pair),
				lane(source, f.dbl, pair + 1U), mxcsr));
		}
		break;
	}
	case FpKind::addsub:
		for (std::size_t index = 0; index < lanes; ++index)
			record(index, binary(f, (index & 1U) == 0 ? FpKind::sub :
				FpKind::add, lane(first, f.dbl, index),
				lane(second, f.dbl, index), mxcsr));
		break;
	case FpKind::dpp:
		steps = dot_product(f, operation.immediate, values, values.result,
			mxcsr);
		break;
	case FpKind::narrow:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto result = narrow(lane(index < 2 ? first : second, true,
				index % 2U), mxcsr);
			set_lane(values.result, false, index, result.bits);
			steps.before[0] |= result.before;
			steps.after[0] |= result.after;
		}
		break;
	case FpKind::fma: {
		const bool negate_product =
			(operation.flags & fp_fma_negate_product) != 0;
		const unsigned addends = (operation.flags >> 1U) & 3U;
		for (std::size_t index = 0; index < lanes; ++index) {
			const bool negate_addend = addends == 1U ||
				(addends == 2U && (index & 1U) == 0) ||
				(addends == 3U && (index & 1U) != 0);
			record(index, fused(f, lane(first, f.dbl, index),
				lane(second, f.dbl, index),
				lane(values.sources[2], f.dbl, index), negate_product,
				negate_addend, mxcsr));
		}
		break;
	}
	case FpKind::cmp:
	case FpKind::comis: {
		const bool signalling = operation.kind == FpKind::cmp ?
			signalling_predicate(operation.immediate,
				(operation.flags & fp_cmp_vex) != 0) :
			(operation.flags & fp_comis_quiet) == 0;
		const auto count = std::min<std::size_t>(lanes,
			16U / operation.element_bytes);
		for (std::size_t index = 0; index < count; ++index)
			steps.before[0] |= compare(f, signalling,
				lane(first, f.dbl, index), lane(second, f.dbl, index), mxcsr);
		break;
	}
	case FpKind::to_int:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto result = to_integer(f,
				packed_lane(values, operation.element_bytes, index),
				(operation.flags & fp_to_int_truncate) != 0,
				(operation.flags & fp_to_int64) != 0, mxcsr);
			steps.before[0] |= result.before;
			steps.after[0] |= result.after;
		}
		break;
	case FpKind::widen:
		for (std::size_t index = 0; index < lanes; ++index) {
			const Format single{false};
			const auto x = packed_lane(values, 4, index);
			if (single.signalling(x))
				steps.before[0] |= invalid;
			else if ((mxcsr & mxcsr_denormals_are_zero) == 0 &&
				single.denormal(x))
				steps.before[0] |= denormal;
		}
		break;
	case FpKind::from_int: {
		const bool wide = (operation.flags & fp_from_int64) != 0;
		const auto count = std::min<std::size_t>(lanes, wide ? 4U : 8U);
		for (std::size_t index = 0; index < count; ++index)
			steps.after[0] |= from_integer(f.dbl,
				packed_lane(values, wide ? 8U : 4U, index), wide);
		break;
	}
	case FpKind::round:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto result = round_integral(f,
				packed_lane(values, operation.element_bytes, index),
				operation.immediate, mxcsr);
			steps.before[0] |= result.before;
			steps.after[0] |= result.after;
		}
		break;
	case FpKind::half_widen:
		/* A signalling NaN half is invalid. */
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto x = packed_lane(values, 2, index);
			if ((x & 0x7fffU) > 0x7c00U && (x & 0x0200U) == 0)
				steps.before[0] |= invalid;
		}
		break;
	case FpKind::half_narrow:
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto result = half_narrowing(packed_lane(values, 4, index),
				operation.immediate, mxcsr);
			steps.before[0] |= result.before;
			steps.after[0] |= result.after;
		}
		break;
	}
	return steps;
}

FpOutcome x86_fp_settle(const FpSteps &steps, std::uint32_t mxcsr) noexcept
{
	const std::uint32_t unmasked = ~(mxcsr >> mask_shift) & 0x3fU;
	FpOutcome outcome{};
	for (std::size_t step = 0; step < FpSteps::count; ++step) {
		outcome.flags |= steps.before[step];
		if ((steps.before[step] & unmasked) != 0) {
			outcome.fault = true;
			return outcome;
		}
		outcome.flags |= steps.after[step];
		if ((steps.after[step] & unmasked) != 0) {
			outcome.fault = true;
			return outcome;
		}
	}
	return outcome;
}

} // namespace uml::cpu::x86_64
