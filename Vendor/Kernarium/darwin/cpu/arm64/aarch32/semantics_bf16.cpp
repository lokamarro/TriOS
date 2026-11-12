// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's BFloat16 extension: the conversions VCVT, VCVTB and
// VCVTT from single precision, the dot product VDOT, the matrix multiply
// accumulate VMMLA, and the widening multiply-adds VFMA. The arithmetic of
// the dot product and the matrix multiply is the architecture's own: each
// product and sum is computed exactly, rounded to odd to a 24-bit
// significand, with denormals flushed and no exceptions, which no host
// instruction is required to match, so it is exact integer arithmetic here.
// The multiply-adds are the host's fused single-precision instruction on the
// exactly widened operands.

#include "cpu/arm64/aarch32/semantics_fp.hpp"
#include "cpu/arm64/aarch32/semantics_vector.hpp"

#include <array>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

constexpr uint32_t default_nan = 0x7fc0'0000U;
constexpr uint32_t overflow_flag = 1U << 2;
constexpr uint32_t underflow_flag = 1U << 3;
constexpr uint32_t input_denormal_flag = 1U << 7;

enum class Kind { zero, finite, infinity, nan };

/* BFUnpack(): a bfloat16 or a single flushed of its denormals, as a sign,
 * and for a finite nonzero value its significand and the exponent of that
 * significand's least significant bit. */
struct Unpacked final {
	Kind kind;
	bool negative;
	uint32_t significand;
	int scale;
};

[[nodiscard]] Unpacked unpack(uint32_t bits, unsigned exponent_shift, unsigned fraction_bits) noexcept
{
	const bool negative = (bits >> (exponent_shift + 8U)) != 0U;
	const uint32_t exponent = bits >> exponent_shift & 0xffU;
	const uint32_t fraction = bits & ((1U << fraction_bits) - 1U);
	if (exponent == 0U)
		return {Kind::zero, negative, 0, 0};
	if (exponent == 0xffU)
		return {fraction == 0U ? Kind::infinity : Kind::nan, negative, 0, 0};
	return {Kind::finite, negative, fraction | 1U << fraction_bits,
		static_cast<int>(exponent) - 127 - static_cast<int>(fraction_bits)};
}

[[nodiscard]] Unpacked unpack_bfloat16(uint16_t bits) noexcept
{
	return unpack(bits, 7, 7);
}
[[nodiscard]] Unpacked unpack_single(uint32_t bits) noexcept
{
	return unpack(bits, 23, 23);
}

[[nodiscard]] constexpr uint32_t signed_zero(bool negative) noexcept
{
	return negative ? 0x8000'0000U : 0U;
}
[[nodiscard]] constexpr uint32_t signed_infinity(bool negative) noexcept
{
	return signed_zero(negative) | 0x7f80'0000U;
}

/* BFRound(): the nonzero value `magnitude` * 2^scale as a single, rounded to
 * odd, with a result below the normal range flushed to zero and one above it
 * an infinity. */
[[nodiscard]] uint32_t round_to_odd(bool negative, u128 magnitude, int scale) noexcept
{
	const auto high = static_cast<uint64_t>(magnitude >> 64);
	const unsigned length = high != 0U ? 128U - static_cast<unsigned>(__builtin_clzll(high))
					   : 64U - static_cast<unsigned>(__builtin_clzll(static_cast<uint64_t>(magnitude)));
	const int shift = static_cast<int>(length) - 24;
	uint32_t significand;
	if (shift <= 0) {
		significand = static_cast<uint32_t>(magnitude << -shift);
	} else {
		significand = static_cast<uint32_t>(magnitude >> shift);
		const bool inexact = (magnitude & ((u128{1} << shift) - 1U)) != 0U;
		if (inexact && (significand & 1U) == 0U)
			++significand;
	}
	const int exponent = scale + shift + 23;
	if (exponent < -126)
		return signed_zero(negative);
	if (exponent + 127 >= 255)
		return signed_infinity(negative);
	return signed_zero(negative) | static_cast<uint32_t>(exponent + 127) << 23 | (significand & 0x7f'ffffU);
}

/* BFMulH(): the product of two bfloat16 values as a single. */
[[nodiscard]] uint32_t multiply(uint16_t first, uint16_t second) noexcept
{
	const Unpacked a = unpack_bfloat16(first);
	const Unpacked b = unpack_bfloat16(second);
	if (a.kind == Kind::nan || b.kind == Kind::nan)
		return default_nan;
	const bool negative = a.negative != b.negative;
	if ((a.kind == Kind::infinity && b.kind == Kind::zero) || (a.kind == Kind::zero && b.kind == Kind::infinity))
		return default_nan;
	if (a.kind == Kind::infinity || b.kind == Kind::infinity)
		return signed_infinity(negative);
	if (a.kind == Kind::zero || b.kind == Kind::zero)
		return signed_zero(negative);
	return round_to_odd(negative, u128{a.significand} * b.significand, a.scale + b.scale);
}

/* FPAdd_BF16(): the sum of two singles, with denormal inputs flushed. */
[[nodiscard]] uint32_t add(uint32_t first, uint32_t second) noexcept
{
	const Unpacked a = unpack_single(first);
	const Unpacked b = unpack_single(second);
	if (a.kind == Kind::nan || b.kind == Kind::nan)
		return default_nan;
	if (a.kind == Kind::infinity && b.kind == Kind::infinity && a.negative != b.negative)
		return default_nan;
	if ((a.kind == Kind::infinity && !a.negative) || (b.kind == Kind::infinity && !b.negative))
		return signed_infinity(false);
	if (a.kind == Kind::infinity || b.kind == Kind::infinity)
		return signed_infinity(true);
	if (a.kind == Kind::zero && b.kind == Kind::zero)
		return a.negative == b.negative ? signed_zero(a.negative) : 0U;
	if (a.kind == Kind::zero)
		return round_to_odd(b.negative, b.significand, b.scale);
	if (b.kind == Kind::zero)
		return round_to_odd(a.negative, a.significand, a.scale);
	/* Align the operands at the smaller scale. When they are more than 100
	 * places apart the smaller one only decides whether the result is
	 * inexact and on which side, so a unit far below the larger one stands
	 * for it. */
	Unpacked high = a.scale >= b.scale ? a : b;
	Unpacked low = a.scale >= b.scale ? b : a;
	if (high.scale - low.scale > 100) {
		low.significand = 1;
		low.scale = high.scale - 100;
	}
	const i128 first_value = static_cast<i128>(high.significand) << (high.scale - low.scale);
	const i128 sum = (high.negative ? -first_value : first_value)
		+ (low.negative ? -i128{low.significand} : i128{low.significand});
	if (sum == 0)
		return 0U;
	return round_to_odd(sum < 0, static_cast<u128>(sum < 0 ? -sum : sum), low.scale);
}

/* BFDotAdd(): addend plus the sum of two bfloat16 products, the standard
 * (non-extended) behavior. */
[[nodiscard]] uint32_t dot_add(uint32_t addend, uint16_t first_a, uint16_t first_b, uint16_t second_a,
	uint16_t second_b) noexcept
{
	return add(addend, add(multiply(first_a, second_a), multiply(first_b, second_b)));
}

/* VDOT, vector and by-element. */
bool vdot(Exec &x, bool by_element)
{
	const Operands &o = x.o;
	const unsigned index = by_element ? static_cast<unsigned>(o.i) : 0U;
	const uint64_t element_source = by_element ? x.d_reg(static_cast<unsigned>(o.m)) : uint64_t{0};
	return for_each_register(x, [&](Raw n, Raw m, Raw d) -> Raw {
		const Raw second = by_element ? element_source : m;
		Raw out = 0;
		for (unsigned e = 0; e < 2U; ++e) {
			const unsigned s = by_element ? index : e;
			out = with_lane(out, e, 32,
				dot_add(static_cast<uint32_t>(lane(d, e, 32)),
					static_cast<uint16_t>(lane(n, 2U * e, 16)),
					static_cast<uint16_t>(lane(n, 2U * e + 1U, 16)),
					static_cast<uint16_t>(lane(second, 2U * s, 16)),
					static_cast<uint16_t>(lane(second, 2U * s + 1U, 16))));
		}
		return out;
	});
}

bool vdot_vector(Exec &x)
{
	return vdot(x, false);
}
bool vdot_element(Exec &x)
{
	return vdot(x, true);
}

/* VMMLA: a 2x4 by 4x2 matrix product of bfloat16 values accumulated into a
 * 2x2 matrix of singles. */
bool vmmla(Exec &x)
{
	const Operands &o = x.o;
	const u128 operand1 = q_reg(x, static_cast<unsigned>(o.n));
	const u128 operand2 = q_reg(x, static_cast<unsigned>(o.m));
	u128 result = q_reg(x, static_cast<unsigned>(o.d));
	const u128 addend = result;
	for (unsigned i = 0; i < 2U; ++i) {
		for (unsigned j = 0; j < 2U; ++j) {
			auto sum = static_cast<uint32_t>(lane128(addend, 2U * i + j, 32));
			for (unsigned k = 0; k < 2U; ++k)
				sum = dot_add(sum, static_cast<uint16_t>(lane128(operand1, 4U * i + 2U * k, 16)),
					static_cast<uint16_t>(lane128(operand1, 4U * i + 2U * k + 1U, 16)),
					static_cast<uint16_t>(lane128(operand2, 4U * j + 2U * k, 16)),
					static_cast<uint16_t>(lane128(operand2, 4U * j + 2U * k + 1U, 16)));
			result = with_lane128(result, 2U * i + j, 32, sum);
		}
	}
	set_q_reg(x, static_cast<unsigned>(o.d), result);
	return true;
}

/* VFMA (BFloat16), vector and by-element: each single of the accumulator
 * gains the product of the bottom or top bfloat16 of the matching pair of
 * each source. */
bool vfma_bf16(Exec &x, bool by_element)
{
	const Operands &o = x.o;
	const unsigned select = static_cast<unsigned>(o.sel);
	const u128 operand1 = q_reg(x, static_cast<unsigned>(o.n));
	const u128 operand2 = by_element ? u128{0} : q_reg(x, static_cast<unsigned>(o.m));
	const uint64_t scalar_source = by_element ? x.d_reg(static_cast<unsigned>(o.m)) : uint64_t{0};
	u128 result = q_reg(x, static_cast<unsigned>(o.d));
	const u128 addend = result;
	fp::run_standard(x.cpu, [&] {
		for (unsigned e = 0; e < 4U; ++e) {
			const auto element1 = static_cast<uint32_t>(lane128(operand1, 2U * e + select, 16)) << 16;
			const auto element2 = by_element
				? static_cast<uint32_t>(lane(scalar_source, static_cast<unsigned>(o.i), 16)) << 16
				: static_cast<uint32_t>(lane128(operand2, 2U * e + select, 16)) << 16;
			result = with_lane128(result, e, 32,
				fp::fused(static_cast<uint32_t>(lane128(addend, e, 32)), element1, element2));
		}
		return 0;
	});
	set_q_reg(x, static_cast<unsigned>(o.d), result);
	return true;
}

bool vfma_bf(Exec &x)
{
	return vfma_bf16(x, false);
}
bool vfma_bfs(Exec &x)
{
	return vfma_bf16(x, true);
}

/* FPConvertBF(): a single to bfloat16 under the flush-to-zero, default-NaN
 * and rounding controls of `fpcr`; the flags it raises go to the FPSR. */
[[nodiscard]] uint16_t convert(CpuState &cpu, uint32_t fpcr, uint32_t operand) noexcept
{
	const bool flush = (fpcr & fp::fpcr_fz) != 0U;
	const unsigned rounding = fpcr >> 22 & 3U;
	const bool negative = operand >> 31 != 0U;
	const auto sign = static_cast<uint16_t>(negative ? 0x8000U : 0U);
	const uint32_t exponent = operand >> 23 & 0xffU;
	const uint32_t fraction = operand & 0x7f'ffffU;
	if (exponent == 0xffU) {
		if (fraction == 0U)
			return sign | 0x7f80U;
		if ((fraction & 0x40'0000U) == 0U)
			cpu.fpsr |= fp::invalid_flag;
		return static_cast<uint16_t>(((fpcr & fp::fpcr_dn) != 0U ? default_nan : operand | 0x40'0000U) >> 16);
	}
	if (exponent == 0U && fraction == 0U)
		return sign;
	if (exponent == 0U && flush) {
		cpu.fpsr |= input_denormal_flag;
		return sign;
	}
	uint32_t biased = exponent;
	const uint32_t significand = exponent == 0U ? fraction : fraction | 1U << 23;
	uint32_t mantissa = significand >> 16;
	const uint32_t remainder = significand & 0xffffU;
	const bool inexact = remainder != 0U;
	uint32_t flags = 0;
	if (biased == 0U && inexact)
		flags |= underflow_flag;
	bool round_up;
	bool overflow_to_infinity;
	switch (rounding) {
	case 1:
		round_up = inexact && !negative;
		overflow_to_infinity = !negative;
		break;
	case 2:
		round_up = inexact && negative;
		overflow_to_infinity = negative;
		break;
	case 3:
		round_up = false;
		overflow_to_infinity = false;
		break;
	default:
		round_up = remainder > 0x8000U || (remainder == 0x8000U && (mantissa & 1U) != 0U);
		overflow_to_infinity = true;
		break;
	}
	if (round_up) {
		++mantissa;
		if (biased == 0U && mantissa == 128U)
			biased = 1;
		if (mantissa == 256U) {
			++biased;
			mantissa = 128;
		}
	}
	uint16_t result;
	bool error = inexact;
	if (biased >= 255U) {
		result = static_cast<uint16_t>(sign | (overflow_to_infinity ? 0x7f80U : 0x7f7fU));
		flags |= overflow_flag;
		error = true;
	} else {
		result = static_cast<uint16_t>(sign | biased << 7 | (mantissa & 0x7fU));
	}
	if (error)
		flags |= fp::inexact_flag;
	cpu.fpsr |= flags;
	return result;
}

/* VCVT: four singles of Qm to four bfloat16 in Dd, under the Standard FPSCR
 * value. */
bool vcvt_bf16(Exec &x)
{
	const u128 source = q_reg(x, static_cast<unsigned>(x.o.m));
	const uint32_t fpcr = fp::standard_fpcr(x.cpu.fpcr);
	uint64_t result = 0;
	for (unsigned e = 0; e < 4U; ++e)
		result = with_lane(result, e, 16, convert(x.cpu, fpcr, static_cast<uint32_t>(lane128(source, e, 32))));
	x.set_d_reg(static_cast<unsigned>(x.o.d), result);
	return true;
}

/* VCVTB and VCVTT: one single to the bottom or top half of Sd, under the
 * FPSCR. */
template<unsigned Shift>
bool vcvt_bf16_half(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	const uint16_t half = convert(x.cpu, x.cpu.fpcr & fpscr_control_mask, x.s_reg(static_cast<unsigned>(x.o.m)));
	x.set_s_reg(d, (x.s_reg(d) & ~(0xffffU << Shift)) | uint32_t{half} << Shift);
	return true;
}

constexpr VectorEntry entries[] = {
	{"VDOT", vdot_vector}, {"VDOT_s", vdot_element}, {"VMMLA", vmmla},
	{"VFMA_bf", vfma_bf}, {"VFMA_bfs", vfma_bfs}, {"VCVT_bfs", vcvt_bf16},
	{"VCVTB_bfs", vcvt_bf16_half<0>}, {"VCVTT_bfs", vcvt_bf16_half<16>},
};

} // namespace

std::span<const VectorEntry> bf16_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
