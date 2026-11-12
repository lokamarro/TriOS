// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's Armv8 floating-point additions: VMAXNM and VMINNM,
// VSEL, VRINT and VCVT with a named rounding, the half-precision moves VINS,
// VMOVX and VMOV, VJCVT, the complex-number forms VCADD and VCMLA, and the
// widening half-precision multiply-adds VFMAL and VFMSL. Each operation is
// the host's own instruction under the FPCR the AArch32 instruction defines
// (semantics_fp.hpp), except VJCVT, which is exact integer arithmetic.

#include "cpu/arm64/aarch32/semantics_fp.hpp"
#include "cpu/arm64/aarch32/semantics_fp_convert.hpp"
#include "cpu/arm64/aarch32/semantics_vector.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

using fp::float_op;
using fp::negated;
using fp::vfp_op;

/* Bits `high` down to `low` of the instruction word, for the encoding fields
 * the decoder does not name. The A32 and T32 forms of these instructions lay
 * the fields out alike. */
[[nodiscard]] unsigned field(const Exec &x, unsigned high, unsigned low) noexcept
{
	return x.inst.word >> low & ((1U << (high - low + 1U)) - 1U);
}

bool vmaxnm(Exec &x)
{
	const bool maximum = x.o.maximum;
	return float_op(x, [maximum](auto, auto n, auto m) {
		return maximum ? fp::max_number(n, m) : fp::min_number(n, m);
	});
}

/* VSEL: a copy of Vn or Vm by a condition on the flags; no flags change. */
bool vsel(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned source = static_cast<unsigned>(x.condition_holds(o.condition) ? o.n : o.m);
	if (o.esize == 64)
		x.set_d_reg(d, x.d_reg(source));
	else
		x.set_s_reg(d, x.s_reg(source) & (o.esize == 16 ? 0xffffU : 0xffff'ffffU));
	return true;
}

/* FPRoundInt() as the instruction names it: with `exact` the host's
 * FRINTX under the FPCR's rounding, otherwise the FRINT of the rounding. */
template<typename Float>
[[nodiscard]] Float round_int(Float value, FPRounding rounding, bool exact) noexcept
{
	if (exact)
		return fp::round_x(value);
	switch (rounding) {
	case FPRounding::POSINF:
		return fp::round_p(value);
	case FPRounding::NEGINF:
		return fp::round_m(value);
	case FPRounding::ZERO:
		return fp::round_z(value);
	case FPRounding::TIEAWAY:
		return fp::round_a(value);
	default:
		return fp::round_n(value);
	}
}

/* VRINTA, VRINTN, VRINTP, VRINTM, VRINTX and VRINTZ, Advanced SIMD. */
bool vrint_asimd(Exec &x)
{
	const FPRounding rounding = x.o.rounding;
	const bool exact = x.o.exact;
	return fp::lanes(x, fp::standard, [=](auto, auto m, auto) { return round_int(m, rounding, exact); });
}

/* VRINTA, VRINTN, VRINTP and VRINTM, VFP: the rounding is the form's. */
bool vrint_fixed(Exec &x)
{
	const FPRounding rounding = x.o.rounding;
	return vfp_op(x, [=](auto, auto, auto m) { return round_int(m, rounding, false); });
}

/* VRINTR and VRINTZ, VFP: zero, or the FPCR's rounding (FRINTI). */
bool vrint_current(Exec &x)
{
	const bool zero = x.o.zero_rounding;
	return vfp_op(x, [=](auto, auto, auto m) { return zero ? fp::round_z(m) : fp::round_i(m); });
}

/* VRINTX, VFP: the FPCR's rounding, raising Inexact. */
bool vrint_exact(Exec &x)
{
	return vfp_op(x, [](auto, auto, auto m) { return fp::round_x(m); });
}

/* VCVTA, VCVTN, VCVTP and VCVTM, Advanced SIMD: a floating-point element to
 * an integer element of its size. */
bool vcvt_rounded_asimd(Exec &x)
{
	const FPRounding rounding = x.o.rounding;
	const bool is_unsigned = x.o.unsigned_;
	if (x.o.esize == 16)
		return fp::lanes_of<uint16_t>(x, [=](uint16_t, uint16_t m, uint16_t) {
			return fp::to_int16_rounded(m, is_unsigned, rounding);
		});
	return fp::lanes_of<uint32_t>(x, [=](uint32_t, uint32_t m, uint32_t) {
		return fp::to_int_rounded(m, is_unsigned, rounding);
	});
}

/* The same for VFP: the 32-bit integer lands in S(d) whatever the source
 * precision. */
bool vcvt_rounded_vfp(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	const FPRounding rounding = o.rounding;
	const bool is_unsigned = o.unsigned_;
	x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
		if (o.esize == 64)
			return fp::to_int_rounded(x.d_reg(m), is_unsigned, rounding);
		if (o.esize == 16)
			return fp::to_int_rounded(static_cast<uint16_t>(x.s_reg(m)), is_unsigned, rounding);
		return fp::to_int_rounded(x.s_reg(m), is_unsigned, rounding);
	}));
	return true;
}

/* VINS: Hm into the top half of Sd. */
bool vins(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	x.set_s_reg(d, (x.s_reg(d) & 0xffffU) | x.s_reg(static_cast<unsigned>(x.o.m)) << 16);
	return true;
}

/* VMOVX: the top half of Sm into Hd. */
bool vmovx(Exec &x)
{
	x.set_s_reg(static_cast<unsigned>(x.o.d), x.s_reg(static_cast<unsigned>(x.o.m)) >> 16);
	return true;
}

/* VMOV between a core register and a half-precision register. */
bool vmov_half(Exec &x)
{
	const Operands &o = x.o;
	const unsigned n = static_cast<unsigned>(o.n);
	if (o.to_arm_register)
		x.set_reg(o.t, x.s_reg(n) & 0xffffU);
	else
		x.set_s_reg(n, x.reg(o.t) & 0xffffU);
	return true;
}

constexpr uint32_t idc_flag = 1U << 7;

/* FPToFixedJS(): a double to a signed 32-bit integer rounding toward zero,
 * wrapping modulo 2^32, with Z set when the conversion was exact and the
 * value is not a negative zero. A NaN, an infinity or a value outside the
 * 32-bit range is Invalid Operation, any other inexact result Inexact, and
 * with flush-to-zero a subnormal input becomes a zero that sets Input
 * Denormal. */
struct JavaScriptConversion final {
	uint32_t value;
	bool exact;
	uint32_t flags;
};

[[nodiscard]] JavaScriptConversion javascript_convert(uint64_t bits, bool flush) noexcept
{
	const bool negative = bits >> 63 != 0U;
	const unsigned exponent = static_cast<unsigned>(bits >> 52 & 0x7ffU);
	const uint64_t fraction = bits & ((uint64_t{1} << 52) - 1U);
	if (exponent == 0x7ffU)
		return {0, false, fp::invalid_flag};
	if (exponent == 0U) {
		/* A zero converts exactly unless it is negative; a subnormal that
		 * the flush turns into a zero is not exact either. */
		if (fraction == 0U)
			return {0, !negative, 0};
		return {0, false, flush ? idc_flag : fp::inexact_flag};
	}
	const uint64_t significand = fraction | uint64_t{1} << 52;
	const int shift = static_cast<int>(exponent) - 1075;
	u128 magnitude;
	bool exact = true;
	bool huge = false;
	if (shift >= 0) {
		if (shift >= 75)
			huge = true;
		magnitude = huge ? u128{0} : static_cast<u128>(significand) << shift;
	} else if (-shift >= 64) {
		magnitude = 0;
		exact = false;
	} else {
		magnitude = significand >> -shift;
		exact = (significand & ((uint64_t{1} << -shift) - 1U)) == 0U;
	}
	const u128 limit = negative ? u128{1} << 31 : (u128{1} << 31) - 1U;
	const bool overflow = huge || magnitude > limit;
	const auto low = static_cast<uint32_t>(magnitude);
	const uint32_t value = negative ? uint32_t{0} - low : low;
	if (overflow)
		return {value, false, fp::invalid_flag};
	if (!exact)
		return {value, false, fp::inexact_flag};
	return {value, true, 0};
}

bool vjcvt(Exec &x)
{
	const JavaScriptConversion result = javascript_convert(
		x.d_reg(static_cast<unsigned>(x.o.m)), (x.cpu.fpcr & fp::fpcr_fz) != 0U);
	x.cpu.fpsr |= result.flags;
	x.cpu.a32_fpscr_nzcv = result.exact ? 0x4000'0000U : 0U;
	x.set_s_reg(static_cast<unsigned>(x.o.d), result.value);
	return true;
}

/* VCADD: the complex addition of the m operand rotated by 90 or 270 degrees,
 * on pairs of real and imaginary elements. */
template<typename Float>
bool vcadd_typed(Exec &x, unsigned rotation)
{
	constexpr unsigned bits = sizeof(Float) * 8U;
	return fp::run_standard(x.cpu, [&]() -> bool {
		return for_each_register(x, [&](uint64_t n, uint64_t m, uint64_t) -> uint64_t {
			uint64_t out = 0;
			for (unsigned pair = 0; pair < 32U / bits; ++pair) {
				const auto n_real = static_cast<Float>(lane(n, 2U * pair, bits));
				const auto n_imag = static_cast<Float>(lane(n, 2U * pair + 1U, bits));
				const auto m_real = static_cast<Float>(lane(m, 2U * pair, bits));
				const auto m_imag = static_cast<Float>(lane(m, 2U * pair + 1U, bits));
				const Float first = rotation == 0U ? negated(m_imag) : m_imag;
				const Float second = rotation == 0U ? m_real : negated(m_real);
				out = with_lane(out, 2U * pair, bits, fp::add(n_real, first));
				out = with_lane(out, 2U * pair + 1U, bits, fp::add(n_imag, second));
			}
			return out;
		});
	});
}

bool vcadd(Exec &x)
{
	const unsigned rotation = field(x, 24, 24);
	return x.o.esize == 16 ? vcadd_typed<uint16_t>(x, rotation) : vcadd_typed<uint32_t>(x, rotation);
}

/* VCMLA: the multiply-accumulate half of a complex multiplication, for the
 * rotation of the instruction. `Scalar` takes the m operand from one pair
 * of elements of D(m). */
template<typename Float, bool Scalar>
bool vcmla_typed(Exec &x, unsigned rotation)
{
	constexpr unsigned bits = sizeof(Float) * 8U;
	const uint64_t scalar_source = Scalar ? x.d_reg(static_cast<unsigned>(x.o.m)) : uint64_t{0};
	const unsigned index = Scalar ? static_cast<unsigned>(x.o.index_value) : 0U;
	return fp::run_standard(x.cpu, [&]() -> bool {
		return for_each_register(x, [&](uint64_t n, uint64_t m, uint64_t d) -> uint64_t {
			const uint64_t source = Scalar ? scalar_source : m;
			uint64_t out = 0;
			for (unsigned pair = 0; pair < 32U / bits; ++pair) {
				const unsigned m_pair = Scalar ? index : pair;
				const auto n_real = static_cast<Float>(lane(n, 2U * pair, bits));
				const auto n_imag = static_cast<Float>(lane(n, 2U * pair + 1U, bits));
				const auto m_real = static_cast<Float>(lane(source, 2U * m_pair, bits));
				const auto m_imag = static_cast<Float>(lane(source, 2U * m_pair + 1U, bits));
				Float element1{}, element2{}, element3{}, element4{};
				switch (rotation) {
				case 0:
					element1 = m_real;
					element2 = n_real;
					element3 = m_imag;
					element4 = n_real;
					break;
				case 1:
					element1 = negated(m_imag);
					element2 = n_imag;
					element3 = m_real;
					element4 = n_imag;
					break;
				case 2:
					element1 = negated(m_real);
					element2 = n_real;
					element3 = negated(m_imag);
					element4 = n_real;
					break;
				default:
					element1 = m_imag;
					element2 = n_imag;
					element3 = negated(m_real);
					element4 = n_imag;
					break;
				}
				const auto d_real = static_cast<Float>(lane(d, 2U * pair, bits));
				const auto d_imag = static_cast<Float>(lane(d, 2U * pair + 1U, bits));
				out = with_lane(out, 2U * pair, bits, fp::fused(d_real, element2, element1));
				out = with_lane(out, 2U * pair + 1U, bits, fp::fused(d_imag, element4, element3));
			}
			return out;
		});
	});
}

bool vcmla(Exec &x)
{
	const unsigned rotation = field(x, 24, 23);
	return x.o.esize == 16 ? vcmla_typed<uint16_t, false>(x, rotation)
			       : vcmla_typed<uint32_t, false>(x, rotation);
}

bool vcmla_s(Exec &x)
{
	const unsigned rotation = field(x, 21, 20);
	return x.o.esize == 16 ? vcmla_typed<uint16_t, true>(x, rotation)
			       : vcmla_typed<uint32_t, true>(x, rotation);
}

/* VFMAL and VFMSL: addend + a * b of half-precision elements, accumulated
 * into single-precision lanes. FPMulAddH() unpacks each half with FPUnpack
 * (AHP ignored, FZ16 flushing a denormal) under the Standard FPSCR value and
 * rounds once. The A64 FMLAL and FMLSL helper's operand expansion does the
 * unpacking exactly, so the host's fused single-precision multiply-add rounds
 * once, under one FPCR install for all the lanes. The elements of the n (and
 * m) operand are the low halves for the first D register of the result and
 * the high halves for the second. */
template<bool Scalar>
bool vfmal(Exec &x)
{
	const Operands &o = x.o;
	const bool subtract = o.sub_op;
	const unsigned regs = static_cast<unsigned>(o.regs);
	const unsigned n = static_cast<unsigned>(o.n);
	const unsigned m = static_cast<unsigned>(o.m);
	const unsigned d = static_cast<unsigned>(o.d);
	const uint64_t operand1 = regs == 2U ? x.d_reg(n) : uint64_t{x.s_reg(n)};
	const uint64_t operand2 = regs == 2U ? x.d_reg(m) : uint64_t{x.s_reg(m)};
	const unsigned index = Scalar ? static_cast<unsigned>(o.index_value) : 0U;
	const uint32_t fpcr = fp::standard_fpcr(x.cpu.fpcr);
	std::array<uint64_t, 2> old{};
	for (unsigned r = 0; r < regs; ++r)
		old[r] = x.d_reg(d + r);
	std::array<uint64_t, 2> result{};
	fp::run(x.cpu, fpcr, [&] {
		for (unsigned r = 0; r < regs; ++r) {
			for (unsigned e = 0; e < 2U; ++e) {
				uint16_t element1 = static_cast<uint16_t>(lane(operand1, 2U * r + e, 16));
				const uint16_t element2 = static_cast<uint16_t>(
					lane(operand2, Scalar ? index : 2U * r + e, 16));
				if (subtract)
					element1 ^= 0x8000U;
				const uint32_t addend = static_cast<uint32_t>(lane(old[r], e, 32));
				result[r] = with_lane(result[r], e, 32,
					fp::fused(addend, ::uml::cpu::arm64::detail::half_fused_operand(element1, fpcr),
						::uml::cpu::arm64::detail::half_fused_operand(element2, fpcr)));
			}
		}
		return 0;
	});
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(d + r, result[r]);
	return true;
}

constexpr VectorEntry entries[] = {
	{"VMAXNM", vmaxnm}, {"VMINNM", vmaxnm}, {"VSELEQ", vsel}, {"VSELGE", vsel},
	{"VSELGT", vsel}, {"VSELVS", vsel},
	{"VRINTA_asimd", vrint_asimd}, {"VRINTN_asimd", vrint_asimd}, {"VRINTP_asimd", vrint_asimd},
	{"VRINTM_asimd", vrint_asimd}, {"VRINTX_asimd", vrint_asimd}, {"VRINTZ_asimd", vrint_asimd},
	{"VRINTA_vfp", vrint_fixed}, {"VRINTN_vfp", vrint_fixed}, {"VRINTP_vfp", vrint_fixed},
	{"VRINTM_vfp", vrint_fixed}, {"VRINTR_vfp", vrint_current}, {"VRINTZ_vfp", vrint_current},
	{"VRINTX_vfp", vrint_exact},
	{"VCVTA_asimd", vcvt_rounded_asimd}, {"VCVTN_asimd", vcvt_rounded_asimd},
	{"VCVTP_asimd", vcvt_rounded_asimd}, {"VCVTM_asimd", vcvt_rounded_asimd},
	{"VCVTA_vfp", vcvt_rounded_vfp}, {"VCVTN_vfp", vcvt_rounded_vfp},
	{"VCVTP_vfp", vcvt_rounded_vfp}, {"VCVTM_vfp", vcvt_rounded_vfp},
	{"VINS", vins}, {"VMOVX", vmovx}, {"VMOV_h", vmov_half}, {"VMOV_toh", vmov_half},
	{"VJCVT", vjcvt}, {"VCADD", vcadd}, {"VCMLA", vcmla}, {"VCMLA_s", vcmla_s},
	{"VFMAL", vfmal<false>}, {"VFMSL", vfmal<false>},
	{"VFMAL_s", vfmal<true>}, {"VFMSL_s", vfmal<true>},
};

} // namespace

std::span<const VectorEntry> armv8_float_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
