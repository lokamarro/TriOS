// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's VFP and Advanced SIMD floating point: arithmetic,
// fused multiply-add, comparison, reciprocal and square-root estimates and
// steps, pairwise forms, and the conversions between precisions, integers
// and fixed point. Each operation is the host's own instruction run under
// the FPCR the AArch32 instruction defines (semantics_fp.hpp): VFP under the
// guest's, Advanced SIMD under the Standard FPSCR value.

#include "cpu/arm64/aarch32/semantics_fp.hpp"
#include "cpu/arm64/aarch32/semantics_fp_convert.hpp"
#include "cpu/arm64/aarch32/semantics_vector.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using std::uint32_t;
using std::uint64_t;

using fp::float_op;
using fp::negated;
using fp::vfp_op;

bool vadd_f(Exec &x)
{
	return float_op(x, [](auto, auto n, auto m) { return fp::add(n, m); });
}
bool vsub_f(Exec &x)
{
	return float_op(x, [](auto, auto n, auto m) { return fp::sub(n, m); });
}
bool vmul_f(Exec &x)
{
	return float_op(x, [](auto, auto n, auto m) { return fp::mul(n, m); });
}
bool vdiv(Exec &x)
{
	return vfp_op(x, [](auto, auto n, auto m) { return fp::div(n, m); });
}
bool vsqrt(Exec &x)
{
	return vfp_op(x, [](auto, auto, auto m) { return fp::sqrt(m); });
}

/* VMLA and VMLS: the product is rounded before the addition. */
bool vmla_f(Exec &x)
{
	const bool add = x.o.add;
	return float_op(x, [add](auto d, auto n, auto m) {
		const auto product = fp::mul(n, m);
		return fp::add(d, add ? product : negated(product));
	});
}

/* VNMLA, VNMLS and VNMUL. */
bool vnmla(Exec &x)
{
	const VFPNegMul kind = x.o.vtype_negmul;
	return vfp_op(x, [kind](auto d, auto n, auto m) {
		const auto product = fp::mul(n, m);
		switch (kind) {
		case VFPNegMul::VNMLA:
			return fp::add(negated(d), negated(product));
		case VFPNegMul::VNMLS:
			return fp::add(negated(d), product);
		default:
			return negated(product);
		}
	});
}

/* VFMA and VFMS (Advanced SIMD or VFP): d + n * m with n negated for VFMS,
 * fused. */
bool vfma(Exec &x)
{
	const bool negate_n = x.o.op1_neg;
	return float_op(x, [negate_n](auto d, auto n, auto m) {
		return fp::fused(d, negate_n ? negated(n) : n, m);
	});
}

/* VFNMA and VFNMS (VFP): -d + n * m, n negated for VFNMA. */
bool vfnma(Exec &x)
{
	const bool negate_n = x.o.op1_neg;
	return vfp_op(x, [negate_n](auto d, auto n, auto m) {
		return fp::fused(negated(d), negate_n ? negated(n) : n, m);
	});
}

bool vabd_f(Exec &x)
{
	return fp::lanes(x, fp::standard, [](auto n, auto m, auto) { return fp::abd(n, m); });
}

bool vmax_min_f(Exec &x)
{
	const bool maximum = x.o.maximum;
	return fp::lanes(x, fp::standard, [maximum](auto n, auto m, auto) {
		return maximum ? fp::max(n, m) : fp::min(n, m);
	});
}

bool vacg(Exec &x)
{
	const bool or_equal = x.o.or_equal;
	return fp::lanes(x, fp::standard, [or_equal](auto n, auto m, auto) {
		return or_equal ? fp::abs_compare_ge(n, m) : fp::abs_compare_gt(n, m);
	});
}

bool vrecps(Exec &x)
{
	const uint32_t guest_fpcr = x.cpu.fpcr;
	return fp::lanes(x, fp::standard, [guest_fpcr](auto n, auto m, auto) {
		return fp::recip_step(n, m, guest_fpcr);
	});
}
bool vrsqrts(Exec &x)
{
	const uint32_t guest_fpcr = x.cpu.fpcr;
	return fp::lanes(x, fp::standard, [guest_fpcr](auto n, auto m, auto) {
		return fp::rsqrt_step(n, m, guest_fpcr);
	});
}

/* The unsigned integer estimates of VRECPE and VRSQRTE: two 32-bit lanes. */
[[nodiscard]] uint64_t unsigned_recip_estimate(uint64_t a) noexcept
{
	uint64_t r;
	__asm__ volatile("fmov d1, %1\n\turecpe v0.2s, v1.2s\n\tfmov %0, d0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}
[[nodiscard]] uint64_t unsigned_rsqrt_estimate(uint64_t a) noexcept
{
	uint64_t r;
	__asm__ volatile("fmov d1, %1\n\tursqrte v0.2s, v1.2s\n\tfmov %0, d0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}

bool vrecpe(Exec &x)
{
	if (!x.o.floating_point)
		return for_each_register(x, [](uint64_t, uint64_t m, uint64_t) { return unsigned_recip_estimate(m); });
	return fp::lanes(x, fp::standard, [](auto, auto m, auto) {
		return fp::recip_estimate(m);
	});
}
bool vrsqrte(Exec &x)
{
	if (!x.o.floating_point)
		return for_each_register(x, [](uint64_t, uint64_t m, uint64_t) { return unsigned_rsqrt_estimate(m); });
	return fp::lanes(x, fp::standard, [](auto, auto m, auto) {
		return fp::rsqrt_estimate(m);
	});
}

/* VPADD, VPMAX and VPMIN on half or single precision pairs: adjacent pairs
 * of n fill the low half of the result, those of m the high half. */
template<typename Float, typename Function>
bool pairwise_typed(Exec &x, Function &&function)
{
	const Operands &o = x.o;
	constexpr unsigned bits = sizeof(Float) * 8U;
	return fp::run_standard(x.cpu, [&]() -> bool {
		const uint64_t n = x.d_reg(static_cast<unsigned>(o.n));
		const uint64_t m = x.d_reg(static_cast<unsigned>(o.m));
		constexpr unsigned half = 32U / bits;
		uint64_t dest = 0;
		for (unsigned e = 0; e < half; ++e) {
			dest = with_lane(dest, e, bits,
				function(static_cast<Float>(lane(n, 2U * e, bits)), static_cast<Float>(lane(n, 2U * e + 1U, bits))));
			dest = with_lane(dest, e + half, bits,
				function(static_cast<Float>(lane(m, 2U * e, bits)), static_cast<Float>(lane(m, 2U * e + 1U, bits))));
		}
		x.set_d_reg(static_cast<unsigned>(o.d), dest);
		return true;
	});
}
template<typename Function>
bool pairwise_f(Exec &x, Function &&function)
{
	if (x.o.esize == 16)
		return pairwise_typed<uint16_t>(x, function);
	return pairwise_typed<uint32_t>(x, function);
}
bool vpadd_f(Exec &x)
{
	return pairwise_f(x, [](auto a, auto b) { return fp::add(a, b); });
}
bool vpmax_min_f(Exec &x)
{
	const bool maximum = x.o.maximum;
	return pairwise_f(x, [maximum](auto a, auto b) {
		return maximum ? fp::max(a, b) : fp::min(a, b);
	});
}

/* VCMP and VCMPE: the result goes to FPSCR[31:28]. */
bool vcmp(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	const bool signal = o.quiet_nan_exc;
	uint32_t flags;
	if (o.esize == 64) {
		const uint64_t a = x.d_reg(d);
		const uint64_t b = o.with_zero ? uint64_t{0} : x.d_reg(m);
		flags = fp::run_effective(x.cpu, [&] { return fp::compare_flags(a, b, signal); });
	} else if (o.esize == 16) {
		const uint16_t a = static_cast<uint16_t>(x.s_reg(d));
		const uint16_t b = o.with_zero ? uint16_t{0} : static_cast<uint16_t>(x.s_reg(m));
		flags = fp::run_effective(x.cpu, [&] { return fp::compare_flags(a, b, signal); });
	} else {
		const uint32_t a = x.s_reg(d);
		const uint32_t b = o.with_zero ? uint32_t{0} : x.s_reg(m);
		flags = fp::run_effective(x.cpu, [&] { return fp::compare_flags(a, b, signal); });
	}
	x.cpu.a32_fpscr_nzcv = flags;
	return true;
}

/* VCVT between double and single precision (VFP). */
bool vcvt_precision(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	if (o.double_to_single)
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] { return fp::double_to_single(x.d_reg(m)); }));
	else
		x.set_d_reg(d, fp::run_effective(x.cpu, [&] { return fp::single_to_double(x.s_reg(m)); }));
	return true;
}

/* VCVTB and VCVTT: half precision in the bottom or top of an S register. */
bool vcvt_half(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	const unsigned shift = static_cast<unsigned>(o.lowbit);
	if (o.convert_from_half) {
		const uint32_t half = x.s_reg(m) >> shift & 0xffffU;
		if (o.uses_double)
			x.set_d_reg(d, fp::run_effective(x.cpu, [&] { return fp::half_to_double(half); }));
		else
			x.set_s_reg(d, fp::run_effective(x.cpu, [&] { return fp::half_to_single(half); }));
		return true;
	}
	const uint32_t half = o.uses_double
		? fp::run_effective(x.cpu, [&] { return fp::double_to_half(x.d_reg(m)); })
		: fp::run_effective(x.cpu, [&] { return fp::single_to_half(x.s_reg(m)); });
	x.set_s_reg(d, (x.s_reg(d) & ~(0xffffU << shift)) | half << shift);
	return true;
}

/* VCVT between a floating-point format and a 32-bit integer (VFP). */
bool vcvt_integer(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	const bool is_unsigned = o.unsigned_;
	if (o.to_integer) {
		const unsigned mode = o.zero_rounding ? 3U : x.cpu.fpcr >> 22 & 3U;
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
			if (o.esize == 64)
				return fp::to_int(x.d_reg(m), is_unsigned, mode);
			if (o.esize == 16)
				return fp::to_int(static_cast<uint16_t>(x.s_reg(m)), is_unsigned, mode);
			return fp::to_int(x.s_reg(m), is_unsigned, mode);
		}));
	} else if (o.esize == 64) {
		x.set_d_reg(d, fp::run_effective(x.cpu, [&] {
			return is_unsigned ? fp::u32_to_double(x.s_reg(m)) : fp::s32_to_double(x.s_reg(m));
		}));
	} else if (o.esize == 16) {
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
			return (is_unsigned ? fp::u32_to_half(x.s_reg(m)) : fp::s32_to_half(x.s_reg(m))) & 0xffffU;
		}));
	} else {
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
			return is_unsigned ? fp::u32_to_single(x.s_reg(m)) : fp::s32_to_single(x.s_reg(m));
		}));
	}
	return true;
}

/* VCVT between single-precision elements and 32-bit integers (Advanced
 * SIMD): to integer rounds toward zero, to floating point to nearest. */
bool vcvt_is(Exec &x)
{
	const bool to_integer = x.o.to_integer;
	const bool is_unsigned = x.o.unsigned_;
	if (x.o.esize == 16)
		return fp::lanes_of<uint16_t>(x, [=](uint16_t, uint16_t m, uint16_t) {
			if (to_integer)
				return is_unsigned ? fp::to_int16_zu(m) : fp::to_int16_zs(m);
			const uint32_t integer = is_unsigned
				? uint32_t{m}
				: static_cast<uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m)));
			return static_cast<uint16_t>(is_unsigned ? fp::u32_to_half(integer) : fp::s32_to_half(integer));
		});
	return fp::lanes_of<uint32_t>(x, [=](uint32_t, uint32_t m, uint32_t) {
		if (to_integer)
			return is_unsigned ? fp::to_int_zu(m) : fp::to_int_zs(m);
		return is_unsigned ? fp::u32_to_single(m) : fp::s32_to_single(m);
	});
}

/* Fixed-point conversions with a 32-bit fixed-point number. */
[[nodiscard]] uint32_t single_to_fixed(uint32_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return is_unsigned ? fp::to_int_zu(a) : fp::to_int_zs(a);
	return is_unsigned ? fp::fixed_convert<fp::SingleToFixedU, uint32_t, uint32_t>(fraction, a)
			   : fp::fixed_convert<fp::SingleToFixedS, uint32_t, uint32_t>(fraction, a);
}
[[nodiscard]] uint32_t double_to_fixed(uint64_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return is_unsigned ? fp::to_int_zu(a) : fp::to_int_zs(a);
	return is_unsigned ? fp::fixed_convert<fp::DoubleToFixedU, uint64_t, uint32_t>(fraction, a)
			   : fp::fixed_convert<fp::DoubleToFixedS, uint64_t, uint32_t>(fraction, a);
}
[[nodiscard]] uint32_t fixed_to_single(uint32_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return is_unsigned ? fp::u32_to_single(a) : fp::s32_to_single(a);
	return is_unsigned ? fp::fixed_convert<fp::FixedUToSingle, uint32_t, uint32_t>(fraction, a)
			   : fp::fixed_convert<fp::FixedSToSingle, uint32_t, uint32_t>(fraction, a);
}
[[nodiscard]] uint64_t fixed_to_double(uint32_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return is_unsigned ? fp::u32_to_double(a) : fp::s32_to_double(a);
	return is_unsigned ? fp::fixed_convert<fp::FixedUToDouble, uint32_t, uint64_t>(fraction, a)
			   : fp::fixed_convert<fp::FixedSToDouble, uint32_t, uint64_t>(fraction, a);
}

[[nodiscard]] uint32_t half_to_fixed(uint32_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return is_unsigned ? fp::to_int_zu(static_cast<uint16_t>(a)) : fp::to_int_zs(static_cast<uint16_t>(a));
	return is_unsigned ? fp::fixed_convert<fp::HalfToFixedU, uint32_t, uint32_t>(fraction, a)
			   : fp::fixed_convert<fp::HalfToFixedS, uint32_t, uint32_t>(fraction, a);
}
/* Half precision to a 16-bit fixed-point number with 1 to 16 fraction bits. */
[[nodiscard]] uint16_t half_to_fixed16(uint16_t a, unsigned fraction, bool is_unsigned) noexcept
{
	return static_cast<uint16_t>(
		is_unsigned ? fp::fixed_convert<fp::HalfToFixed16U, uint32_t, uint32_t, 16>(fraction, a)
			    : fp::fixed_convert<fp::HalfToFixed16S, uint32_t, uint32_t, 16>(fraction, a));
}
[[nodiscard]] uint32_t fixed_to_half(uint32_t a, unsigned fraction, bool is_unsigned) noexcept
{
	if (fraction == 0U)
		return (is_unsigned ? fp::u32_to_half(a) : fp::s32_to_half(a)) & 0xffffU;
	return is_unsigned ? fp::fixed_convert<fp::FixedUToHalf, uint32_t, uint32_t>(fraction, a) & 0xffffU
			   : fp::fixed_convert<fp::FixedSToHalf, uint32_t, uint32_t>(fraction, a) & 0xffffU;
}

/* A 16-bit integer extended to 32 bits by its type. */
[[nodiscard]] uint32_t extend16(uint32_t value, bool is_unsigned) noexcept
{
	return is_unsigned ? value & 0xffffU
			   : static_cast<uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

/* VCVT between single-precision elements and fixed point (Advanced SIMD). */
bool vcvt_xs(Exec &x)
{
	const bool to_fixed = x.o.to_fixed;
	const bool is_unsigned = x.o.unsigned_;
	const unsigned fraction = static_cast<unsigned>(x.o.frac_bits);
	if (x.o.esize == 16)
		return fp::lanes_of<uint16_t>(x, [=](uint16_t, uint16_t m, uint16_t) {
			if (to_fixed)
				return half_to_fixed16(m, fraction, is_unsigned);
			return static_cast<uint16_t>(fixed_to_half(extend16(m, is_unsigned), fraction, is_unsigned));
		});
	return fp::lanes_of<uint32_t>(x, [=](uint32_t, uint32_t m, uint32_t) {
		return to_fixed ? single_to_fixed(m, fraction, is_unsigned)
				: fixed_to_single(m, fraction, is_unsigned);
	});
}

/* VCVT between a floating-point format and 16- or 32-bit fixed point in the
 * same VFP register. The 16-bit forms convert through 32 bits and saturate
 * to 16: a result out of range is an invalid operation, not inexact. */
bool vcvt_fixed(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const bool is_unsigned = o.unsigned_;
	const unsigned fraction = static_cast<unsigned>(o.frac_bits);
	const unsigned bits = static_cast<unsigned>(o.size);
	const unsigned precision = static_cast<unsigned>(o.fp_size);
	if (!o.to_fixed) {
		/* The integer is the low `bits` of the register, extended; the
		 * conversion rounds to nearest even whatever the FPSCR says. */
		const uint32_t raw = precision == 64U ? static_cast<uint32_t>(x.d_reg(d)) : x.s_reg(d);
		const uint32_t integer = bits == 16U ? extend16(raw, is_unsigned) : raw;
		const uint32_t local = x.cpu.fpcr & fpscr_control_mask & ~fp::fpcr_rounding;
		if (precision == 64U)
			x.set_d_reg(d, fp::run(x.cpu, local, [&] { return fixed_to_double(integer, fraction, is_unsigned); }));
		else if (precision == 16U)
			x.set_s_reg(d, fp::run(x.cpu, local, [&] { return fixed_to_half(integer, fraction, is_unsigned); }));
		else
			x.set_s_reg(d, fp::run(x.cpu, local, [&] { return fixed_to_single(integer, fraction, is_unsigned); }));
		return true;
	}
	const uint32_t local = x.cpu.fpcr & fpscr_control_mask;
	const auto result = ::uml::cpu::arm64::detail::run_with_local_fpcr(local, [&] {
		if (precision == 64U)
			return double_to_fixed(x.d_reg(d), fraction, is_unsigned);
		if (precision == 16U)
			return half_to_fixed(x.s_reg(d) & 0xffffU, fraction, is_unsigned);
		return single_to_fixed(x.s_reg(d), fraction, is_unsigned);
	});
	uint32_t value = result.value;
	std::uint32_t raised = result.fpsr;
	if (bits == 16U) {
		const std::int64_t wide = is_unsigned ? std::int64_t{value} : std::int64_t{static_cast<std::int32_t>(value)};
		const std::int64_t upper = is_unsigned ? 0xffff : 0x7fff;
		const std::int64_t lower = is_unsigned ? 0 : -0x8000;
		if (wide > upper || wide < lower) {
			value = static_cast<uint32_t>(wide > upper ? upper : lower);
			raised = (raised & ~fp::inexact_flag) | fp::invalid_flag;
		}
	}
	/* Extend{32} or Extend{64} of the result by the type. */
	const uint32_t extended = bits == 16U ? extend16(value, is_unsigned) : value;
	if (precision == 64U)
		x.set_d_reg(d, is_unsigned ? uint64_t{extended}
					   : static_cast<uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(extended))));
	else
		x.set_s_reg(d, extended);
	x.cpu.fpsr |= raised;
	return true;
}

/* VCVT (half precision, Advanced SIMD): four lanes between half-precision
 * D elements and single-precision Q elements. */
bool vcvt_half_simd(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	return fp::run_standard(x.cpu, [&]() -> bool {
		if (o.half_to_single) {
			const uint64_t source = x.d_reg(m);
			u128 out = 0;
			for (unsigned e = 0; e < 4U; ++e)
				out = with_lane128(out, e, 32U,
					fp::half_to_single(static_cast<uint32_t>(lane(source, e, 16U))));
			set_q_reg(x, d, out);
		} else {
			const u128 source = q_reg(x, m);
			uint64_t out = 0;
			for (unsigned e = 0; e < 4U; ++e)
				out = with_lane(out, e, 16U,
					fp::single_to_half(static_cast<uint32_t>(lane128(source, e, 32U))));
			x.set_d_reg(d, out);
		}
		return true;
	});
}

constexpr VectorEntry entries[] = {
	{"VADD_f", vadd_f}, {"VSUB_f", vsub_f}, {"VMUL_f", vmul_f}, {"VDIV", vdiv},
	{"VSQRT", vsqrt}, {"VMLA_f", vmla_f}, {"VMLS_f", vmla_f}, {"VNMLA", vnmla},
	{"VNMLS", vnmla}, {"VNMUL", vnmla}, {"VFMA", vfma}, {"VFMS", vfma},
	{"VFNMA", vfnma}, {"VFNMS", vfnma}, {"VABD_f", vabd_f}, {"VMAX_f", vmax_min_f},
	{"VMIN_f", vmax_min_f}, {"VACGE", vacg}, {"VACGT", vacg}, {"VRECPS", vrecps},
	{"VRSQRTS", vrsqrts}, {"VRECPE", vrecpe}, {"VRSQRTE", vrsqrte},
	{"VPADD_f", vpadd_f}, {"VPMAX_f", vpmax_min_f}, {"VPMIN_f", vpmax_min_f},
	{"VCMP", vcmp}, {"VCMPE", vcmp}, {"VCVT_ds", vcvt_precision},
	{"VCVT_sd", vcvt_precision}, {"VCVTB", vcvt_half}, {"VCVTT", vcvt_half},
	{"VCVT_uiv", vcvt_integer}, {"VCVT_siv", vcvt_integer}, {"VCVT_vi", vcvt_integer},
	{"VCVTR_uiv", vcvt_integer}, {"VCVTR_siv", vcvt_integer}, {"VCVT_is", vcvt_is},
	{"VCVT_xs", vcvt_xs}, {"VCVT_toxv", vcvt_fixed}, {"VCVT_xv", vcvt_fixed},
	{"VCVT_hs", vcvt_half_simd}, {"VCVT_sh", vcvt_half_simd},
};

} // namespace

std::span<const VectorEntry> vector_float_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
