// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's Advanced SIMD integer arithmetic: the same-size,
// long, wide and narrow forms, the saturating and rounding forms, the
// comparisons, the pairwise forms and the by-scalar multiplies. The stems
// that share a pseudocode body with the floating-point forms (VCEQ, VABS,
// VMUL by scalar and so on) are here and take the floating-point path
// through semantics_fp.hpp.

#include "cpu/arm64/aarch32/semantics_fp.hpp"
#include "cpu/arm64/aarch32/semantics_vector.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

[[nodiscard]] i128 element_value(const Exec &x, Raw raw) noexcept
{
	return lane_value(raw, element_bits(x), !x.o.unsigned_);
}

bool vadd_i(Exec &x)
{
	return lanewise(x, [](Raw n, Raw m, Raw) { return n + m; });
}
bool vsub_i(Exec &x)
{
	return lanewise(x, [](Raw n, Raw m, Raw) { return n - m; });
}

bool vhadd(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool add = x.o.add;
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		const i128 a = element_value(x, n);
		const i128 b = element_value(x, m);
		return lane_bits((add ? a + b : a - b) >> 1, bits);
	});
}

bool vrhadd(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		return lane_bits((element_value(x, n) + element_value(x, m) + 1) >> 1, bits);
	});
}

bool vabd_i(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		return lane_bits(absolute(element_value(x, n) - element_value(x, m)), bits);
	});
}

bool vaba(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw n, Raw m, Raw d) {
		return lane_bits(absolute(element_value(x, n) - element_value(x, m)) + d, bits);
	});
}

bool vmax_min_i(Exec &x)
{
	const bool maximum = x.o.maximum;
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		const i128 a = element_value(x, n);
		const i128 b = element_value(x, m);
		return lane_bits(maximum ? (a > b ? a : b) : (a < b ? a : b), element_bits(x));
	});
}

bool vmul_i(Exec &x)
{
	if (x.o.polynomial)
		return lanewise(x, [](Raw n, Raw m, Raw) {
			return static_cast<Raw>(polynomial_multiply(n, m));
		});
	return lanewise(x, [](Raw n, Raw m, Raw) { return n * m; });
}

bool vmla_i(Exec &x)
{
	const bool add = x.o.add;
	return lanewise(x, [&](Raw n, Raw m, Raw d) { return add ? d + n * m : d - n * m; });
}

bool vtst(Exec &x)
{
	const Raw ones = lane_mask(element_bits(x));
	return lanewise(x, [&](Raw n, Raw m, Raw) { return (n & m) != 0U ? ones : Raw{0}; });
}

/* The saturating forms. A saturated element sets QC. */
bool vqadd_sub(Exec &x, bool subtract)
{
	const unsigned bits = element_bits(x);
	const bool is_unsigned = x.o.unsigned_;
	QcTally qc;
	const bool completed = lanewise(x, [&](Raw n, Raw m, Raw) {
		const i128 a = lane_value(n, bits, !is_unsigned);
		const i128 b = lane_value(m, bits, !is_unsigned);
		return qc.take(sat_q(subtract ? a - b : a + b, bits, is_unsigned));
	});
	return qc.finish(x, completed);
}
bool vqadd(Exec &x)
{
	return vqadd_sub(x, false);
}
bool vqsub(Exec &x)
{
	return vqadd_sub(x, true);
}

bool vqabs_neg(Exec &x, bool negate)
{
	const unsigned bits = element_bits(x);
	QcTally qc;
	const bool completed = lanewise(x, [&](Raw, Raw m, Raw) {
		const i128 value = lane_value(m, bits, true);
		return qc.take(sat_q(negate ? -value : absolute(value), bits, false));
	});
	return qc.finish(x, completed);
}
bool vqabs(Exec &x)
{
	return vqabs_neg(x, false);
}
bool vqneg(Exec &x)
{
	return vqabs_neg(x, true);
}

/* VQDMULH and VQRDMULH, vector or by scalar. */
bool vqdmulh_family(Exec &x, bool round)
{
	const unsigned bits = element_bits(x);
	QcTally qc;
	const auto element = [&](Raw n, Raw m, Raw) {
		const i128 doubled = 2 * lane_value(n, bits, true) * lane_value(m, bits, true);
		return qc.take(sat_q(shift_right(doubled, bits, round), bits, false));
	};
	return qc.finish(x, x.o.scalar_form ? lanewise_scalar(x, element) : lanewise(x, element));
}
bool vqdmulh(Exec &x)
{
	return vqdmulh_family(x, false);
}
bool vqrdmulh(Exec &x)
{
	return vqdmulh_family(x, true);
}

bool vclz(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw, Raw m, Raw) {
		return m == 0U ? Raw{bits} : Raw(__builtin_clzll(m)) - (64U - bits);
	});
}

bool vcls(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw, Raw m, Raw) {
		const Raw folded = (m >> 1 ^ m) & lane_mask(bits - 1U);
		return folded == 0U ? Raw{bits - 1U} : Raw(__builtin_clzll(folded)) - (64U - (bits - 1U));
	});
}

bool vcnt(Exec &x)
{
	return lanewise(x, [](Raw, Raw m, Raw) { return Raw(__builtin_popcountll(m)); });
}

/* The comparisons put all ones in a true element. */
bool vceq_r(Exec &x)
{
	if (x.o.int_operation) {
		const Raw ones = lane_mask(element_bits(x));
		return lanewise(x, [&](Raw n, Raw m, Raw) { return n == m ? ones : Raw{0}; });
	}
	return fp::lanes(x, fp::standard, [](auto n, auto m, auto) { return fp::compare_eq(n, m); });
}

bool vcge_r(Exec &x)
{
	const unsigned bits = element_bits(x);
	const Raw ones = lane_mask(bits);
	switch (x.o.vtype_vcge) {
	case VCGEType::signed_:
		return lanewise(x, [&](Raw n, Raw m, Raw) {
			return lane_value(n, bits, true) >= lane_value(m, bits, true) ? ones : Raw{0};
		});
	case VCGEType::unsigned_:
		return lanewise(x, [&](Raw n, Raw m, Raw) { return n >= m ? ones : Raw{0}; });
	default:
		return fp::lanes(x, fp::standard, [](auto n, auto m, auto) { return fp::compare_ge(n, m); });
	}
}

bool vcgt_r(Exec &x)
{
	const unsigned bits = element_bits(x);
	const Raw ones = lane_mask(bits);
	switch (x.o.vtype_vcgt) {
	case VCGTtype::signed_:
		return lanewise(x, [&](Raw n, Raw m, Raw) {
			return lane_value(n, bits, true) > lane_value(m, bits, true) ? ones : Raw{0};
		});
	case VCGTtype::unsigned_:
		return lanewise(x, [&](Raw n, Raw m, Raw) { return n > m ? ones : Raw{0}; });
	default:
		return fp::lanes(x, fp::standard, [](auto n, auto m, auto) { return fp::compare_gt(n, m); });
	}
}

/* The comparisons with zero, signed integer or floating point. */
enum class ZeroTest { eq, ge, gt, le, lt };

template<ZeroTest Test>
bool compare_with_zero(Exec &x)
{
	if (x.o.floating_point)
		return fp::lanes(x, fp::standard, [](auto, auto m, auto) {
			const decltype(m) zero{0};
			switch (Test) {
			case ZeroTest::eq:
				return fp::compare_eq(m, zero);
			case ZeroTest::ge:
				return fp::compare_ge(m, zero);
			case ZeroTest::gt:
				return fp::compare_gt(m, zero);
			case ZeroTest::le:
				return fp::compare_ge(zero, m);
			default:
				return fp::compare_gt(zero, m);
			}
		});
	const unsigned bits = element_bits(x);
	const Raw ones = lane_mask(bits);
	return lanewise(x, [&](Raw, Raw m, Raw) {
		const i128 value = lane_value(m, bits, true);
		bool passed;
		switch (Test) {
		case ZeroTest::eq:
			passed = value == 0;
			break;
		case ZeroTest::ge:
			passed = value >= 0;
			break;
		case ZeroTest::gt:
			passed = value > 0;
			break;
		case ZeroTest::le:
			passed = value <= 0;
			break;
		default:
			passed = value < 0;
			break;
		}
		return passed ? ones : Raw{0};
	});
}

/* VABS and VNEG: Advanced SIMD integer or floating point, or VFP. The
 * floating-point forms only change the sign bit. */
template<bool Negate>
bool vabs_neg(Exec &x)
{
	const Operands &o = x.o;
	if (o.advsimd && !o.floating_point) {
		const unsigned bits = element_bits(x);
		return lanewise(x, [&](Raw, Raw m, Raw) {
			const i128 value = lane_value(m, bits, true);
			return lane_bits(Negate ? -value : absolute(value), bits);
		});
	}
	const auto apply = [](auto value, auto sign) { return Negate ? value ^ sign : value & ~sign; };
	if (o.advsimd) {
		const Raw sign = o.esize == 16 ? 0x8000'8000'8000'8000ULL : 0x8000'0000'8000'0000ULL;
		return for_each_register(x, [&](Raw, Raw m, Raw) -> Raw { return apply(m, sign); });
	}
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned m = static_cast<unsigned>(o.m);
	if (o.esize == 64)
		x.set_d_reg(d, apply(x.d_reg(m), Raw{0x8000'0000'0000'0000ULL}));
	else if (o.esize == 16)
		x.set_s_reg(d, apply(x.s_reg(m) & 0xffffU, std::uint32_t{0x8000U}));
	else
		x.set_s_reg(d, apply(x.s_reg(m), std::uint32_t{0x8000'0000U}));
	return true;
}

/* VMUL, VMLA and VMLS by scalar: integer, or floating point with the
 * Standard FPSCR value. */
bool vmul_s(Exec &x)
{
	if (x.o.floating_point) {
		const unsigned bits = x.o.esize == 16 ? 16U : 32U;
		const std::uint32_t scalar = static_cast<std::uint32_t>(lane(
			x.d_reg(static_cast<unsigned>(x.o.m)), static_cast<unsigned>(x.o.index_value), bits));
		return fp::lanes(x, fp::standard, [&](auto n, auto, auto) {
			return fp::mul(n, static_cast<decltype(n)>(scalar));
		});
	}
	return lanewise_scalar(x, [](Raw n, Raw m, Raw) { return n * m; });
}

bool vmla_s(Exec &x)
{
	const bool add = x.o.add;
	if (x.o.floating_point) {
		const unsigned bits = x.o.esize == 16 ? 16U : 32U;
		const std::uint32_t scalar = static_cast<std::uint32_t>(lane(
			x.d_reg(static_cast<unsigned>(x.o.m)), static_cast<unsigned>(x.o.index_value), bits));
		return fp::lanes(x, fp::standard, [&](auto n, auto, auto d) {
			using Float = decltype(n);
			const Float product = fp::mul(n, static_cast<Float>(scalar));
			const Float sign = Float{1} << (sizeof(Float) * 8U - 1U);
			return fp::add(d, add ? product : static_cast<Float>(product ^ sign));
		});
	}
	return lanewise_scalar(x, [&](Raw n, Raw m, Raw d) { return add ? d + n * m : d - n * m; });
}

bool vaddl(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool wide = x.o.is_vaddw;
	const bool is_signed = !x.o.unsigned_;
	return long_lanes(x, wide, false, [&](Raw n, Raw m, Raw) {
		const i128 a = lane_value(n, wide ? 2U * bits : bits, is_signed);
		return lane_bits(a + lane_value(m, bits, is_signed), 2U * bits);
	});
}

bool vsubl(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool wide = x.o.is_vsubw;
	const bool is_signed = !x.o.unsigned_;
	return long_lanes(x, wide, false, [&](Raw n, Raw m, Raw) {
		const i128 a = lane_value(n, wide ? 2U * bits : bits, is_signed);
		return lane_bits(a - lane_value(m, bits, is_signed), 2U * bits);
	});
}

/* VABDL, and VABAL which also adds the old destination. */
template<bool Accumulate>
bool vabdl(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool is_signed = !x.o.unsigned_;
	return long_lanes(x, false, false, [&](Raw n, Raw m, Raw d) {
		const i128 difference =
			absolute(lane_value(n, bits, is_signed) - lane_value(m, bits, is_signed));
		return lane_bits(Accumulate ? difference + d : difference, 2U * bits);
	});
}

/* VMULL, VMLAL and VMLSL by vector or by scalar, integer or polynomial. */
bool vmull(Exec &x, bool scalar)
{
	const Operands &o = x.o;
	const unsigned bits = element_bits(x);
	if (o.polynomial) {
		if (bits == 64U) {
			set_q_reg(x, static_cast<unsigned>(o.d),
				polynomial_multiply(x.d_reg(static_cast<unsigned>(o.n)),
					x.d_reg(static_cast<unsigned>(o.m))));
			return true;
		}
		return long_lanes(x, false, false, [](Raw n, Raw m, Raw) {
			return static_cast<Raw>(polynomial_multiply(n, m));
		});
	}
	const bool is_signed = !o.unsigned_;
	return long_lanes(x, false, scalar, [&](Raw n, Raw m, Raw) {
		return lane_bits(lane_value(n, bits, is_signed) * lane_value(m, bits, is_signed), 2U * bits);
	});
}
bool vmull_i(Exec &x)
{
	return vmull(x, false);
}
bool vmull_s(Exec &x)
{
	return vmull(x, true);
}

bool vmlal(Exec &x, bool scalar)
{
	const unsigned bits = element_bits(x);
	const bool is_signed = !x.o.unsigned_;
	const bool add = x.o.add;
	return long_lanes(x, false, scalar, [&](Raw n, Raw m, Raw d) {
		const i128 product = lane_value(n, bits, is_signed) * lane_value(m, bits, is_signed);
		return lane_bits(lane_value(d, 2U * bits, false) + (add ? product : -product), 2U * bits);
	});
}
bool vmlal_i(Exec &x)
{
	return vmlal(x, false);
}
bool vmlal_s(Exec &x)
{
	return vmlal(x, true);
}

/* VQDMULL, VQDMLAL and VQDMLSL, vector or by scalar. */
bool vqdmull(Exec &x)
{
	const unsigned bits = element_bits(x);
	QcTally qc;
	const bool completed = long_lanes(x, false, x.o.scalar_form, [&](Raw n, Raw m, Raw) {
		return qc.take(
			sat_q(2 * lane_value(n, bits, true) * lane_value(m, bits, true), 2U * bits, false));
	});
	return qc.finish(x, completed);
}

bool vqdmlal(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool add = x.o.add;
	QcTally qc;
	const bool completed = long_lanes(x, false, x.o.scalar_form, [&](Raw n, Raw m, Raw d) {
		const Raw product = qc.take(
			sat_q(2 * lane_value(n, bits, true) * lane_value(m, bits, true), 2U * bits, false));
		const i128 accumulated = lane_value(d, 2U * bits, true)
			+ (add ? 1 : -1) * lane_value(product, 2U * bits, true);
		return qc.take(sat_q(accumulated, 2U * bits, false));
	});
	return qc.finish(x, completed);
}

bool vmovl(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool is_signed = !x.o.unsigned_;
	return long_lanes(x, false, false, [&](Raw, Raw m, Raw) {
		return lane_bits(lane_value(m, bits, is_signed), 2U * bits);
	});
}

bool vmovn(Exec &x)
{
	return narrow_lanes(x, [](Raw, Raw m) { return m; });
}

bool vqmovn(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool source_unsigned = x.o.src_unsigned;
	const bool destination_unsigned = x.o.dest_unsigned;
	QcTally qc;
	const bool completed = narrow_lanes(x, [&](Raw, Raw m) {
		return qc.take(
			sat_q(lane_value(m, 2U * bits, !source_unsigned), bits, destination_unsigned));
	});
	return qc.finish(x, completed);
}

/* VADDHN, VSUBHN and the rounding forms: the high half of the sum or
 * difference of the wide elements. */
template<bool Subtract, bool Round>
bool vaddhn(Exec &x)
{
	const unsigned bits = element_bits(x);
	return narrow_lanes(x, [&](Raw n, Raw m) {
		const Raw wide = (Subtract ? n - m : n + m) & lane_mask(2U * bits);
		return static_cast<Raw>(shift_right(static_cast<i128>(wide), bits, Round));
	});
}

/* VPADDL and VPADAL: the sum of adjacent pairs, accumulated for VPADAL. */
template<bool Accumulate>
bool vpaddl(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned pairs = 32U / bits;
	const bool is_signed = !x.o.unsigned_;
	return for_each_register(x, [&](Raw, Raw m, Raw d) -> Raw {
		Raw out = 0;
		for (unsigned e = 0; e < pairs; ++e) {
			const i128 sum = lane_value(lane(m, 2U * e, bits), bits, is_signed)
				+ lane_value(lane(m, 2U * e + 1U, bits), bits, is_signed);
			Raw wide = lane_bits(sum, 2U * bits);
			if (Accumulate)
				wide += lane(d, e, 2U * bits);
			out = with_lane(out, e, 2U * bits, wide);
		}
		return out;
	});
}

/* The pairwise forms on D registers: adjacent pairs of n fill the low half
 * of the result, those of m the high half. */
template<typename Function>
bool pairwise(Exec &x, Function &&function)
{
	const Operands &o = x.o;
	const unsigned bits = element_bits(x);
	const unsigned half = 32U / bits;
	const Raw n = x.d_reg(static_cast<unsigned>(o.n));
	const Raw m = x.d_reg(static_cast<unsigned>(o.m));
	Raw dest = 0;
	for (unsigned e = 0; e < half; ++e) {
		dest = with_lane(dest, e, bits, function(lane(n, 2U * e, bits), lane(n, 2U * e + 1U, bits)));
		dest = with_lane(dest, e + half, bits,
			function(lane(m, 2U * e, bits), lane(m, 2U * e + 1U, bits)));
	}
	x.set_d_reg(static_cast<unsigned>(o.d), dest);
	return true;
}

bool vpadd_i(Exec &x)
{
	return pairwise(x, [](Raw a, Raw b) { return a + b; });
}

bool vpmax_min_i(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool maximum = x.o.maximum;
	const bool is_signed = !x.o.unsigned_;
	return pairwise(x, [&](Raw a, Raw b) {
		const i128 first = lane_value(a, bits, is_signed);
		const i128 second = lane_value(b, bits, is_signed);
		return lane_bits(maximum ? (first > second ? first : second)
					 : (first < second ? first : second),
			bits);
	});
}

constexpr VectorEntry entries[] = {
	{"VADD_i", vadd_i}, {"VSUB_i", vsub_i}, {"VHADD", vhadd}, {"VHSUB", vhadd},
	{"VRHADD", vrhadd}, {"VABD_i", vabd_i}, {"VABA", vaba}, {"VMAX_i", vmax_min_i},
	{"VMIN_i", vmax_min_i}, {"VMUL_i", vmul_i}, {"VMLA_i", vmla_i},
	{"VMLS_i", vmla_i}, {"VTST", vtst}, {"VQADD", vqadd}, {"VQSUB", vqsub},
	{"VQABS", vqabs}, {"VQNEG", vqneg}, {"VQDMULH", vqdmulh},
	{"VQRDMULH", vqrdmulh}, {"VCLZ", vclz}, {"VCLS", vcls}, {"VCNT", vcnt},
	{"VCEQ_r", vceq_r}, {"VCGE_r", vcge_r}, {"VCGT_r", vcgt_r},
	{"VCEQ_i", compare_with_zero<ZeroTest::eq>}, {"VCGE_i", compare_with_zero<ZeroTest::ge>},
	{"VCGT_i", compare_with_zero<ZeroTest::gt>}, {"VCLE_i", compare_with_zero<ZeroTest::le>},
	{"VCLT_i", compare_with_zero<ZeroTest::lt>}, {"VABS", vabs_neg<false>},
	{"VNEG", vabs_neg<true>}, {"VMUL_s", vmul_s}, {"VMLA_s", vmla_s},
	{"VMLS_s", vmla_s}, {"VADDL", vaddl}, {"VADDW", vaddl}, {"VSUBL", vsubl},
	{"VSUBW", vsubl}, {"VABAL", vabdl<true>}, {"VABDL_i", vabdl<false>}, {"VMULL_i", vmull_i},
	{"VMULL_s", vmull_s}, {"VMLAL_i", vmlal_i}, {"VMLSL_i", vmlal_i},
	{"VMLAL_s", vmlal_s}, {"VMLSL_s", vmlal_s}, {"VQDMULL", vqdmull},
	{"VQDMLAL", vqdmlal}, {"VQDMLSL", vqdmlal}, {"VMOVL", vmovl},
	{"VMOVN", vmovn}, {"VQMOVN", vqmovn}, {"VQMOVUN", vqmovn},
	{"VADDHN", vaddhn<false, false>}, {"VSUBHN", vaddhn<true, false>},
	{"VRADDHN", vaddhn<false, true>}, {"VRSUBHN", vaddhn<true, true>},
	{"VPADDL", vpaddl<false>}, {"VPADAL", vpaddl<true>}, {"VPADD_i", vpadd_i},
	{"VPMAX_i", vpmax_min_i}, {"VPMIN_i", vpmax_min_i},
};

} // namespace

std::span<const VectorEntry> neon_integer_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
