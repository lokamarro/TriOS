// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/semantics_vector.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"

#include <cstdint>

/*
 * The host's own floating-point instructions as the library's VFP and
 * Advanced SIMD arithmetic: each operation is one A64 scalar instruction on
 * bit patterns, run with the FPCR the AArch32 instruction defines (the
 * guest FPSCR's control bits for VFP, the Standard FPSCR value for Advanced
 * SIMD) and the cumulative flags it raises merged into the guest FPSR.
 * Overloads on std::uint16_t, std::uint32_t and std::uint64_t select the
 * half, single and double precision instruction.
 */

namespace uml::cpu::arm64::aarch32::detail::fp {

inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpcr_rounding = 3U << 22;
/* Cumulative exception flags in the FPSR. */
inline constexpr std::uint32_t invalid_flag = 1U << 0;
inline constexpr std::uint32_t inexact_flag = 1U << 4;

/* The FPCR an Advanced SIMD floating-point operation runs under: the
 * Standard FPSCR value, flush-to-zero and default NaN with round to nearest,
 * keeping the half-precision format and flush controls. */
[[nodiscard]] constexpr std::uint32_t standard_fpcr(std::uint32_t guest) noexcept
{
	return fpcr_fz | fpcr_dn | (guest & (fpcr_ahp | fpcr_fz16));
}

/* Runs `function` with `local` installed as the host FPCR and FPSR clear,
 * restores both, and merges the flags it raised into the guest FPSR. The
 * function issues its floating-point instructions as asm volatile, as the
 * operations below do. */
template<typename Function>
inline auto run(CpuState &cpu, std::uint32_t local, Function &&function) noexcept
{
	const auto result = ::uml::cpu::arm64::detail::run_with_local_fpcr(
		std::uint64_t{local}, static_cast<Function &&>(function));
	cpu.fpsr |= result.fpsr;
	return result.value;
}

/* Run under the guest FPSCR (VFP) or the Standard FPSCR value (Advanced
 * SIMD). */
template<typename Function>
inline auto run_effective(CpuState &cpu, Function &&function) noexcept
{
	return run(cpu, cpu.fpcr & fpscr_control_mask, static_cast<Function &&>(function));
}
template<typename Function>
inline auto run_standard(CpuState &cpu, Function &&function) noexcept
{
	return run(cpu, standard_fpcr(cpu.fpcr), static_cast<Function &&>(function));
}

/* One operation on bit patterns, in half, single and double precision. The
 * half-precision form needs the host's FP16 arithmetic, which every
 * supported host has. */
#define UML_A32_FP_BINARY(name, insn)                                                      \
	[[nodiscard]] inline std::uint16_t name(std::uint16_t a, std::uint16_t b) noexcept \
	{                                                                                  \
		std::uint32_t r;                                                           \
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\t" insn " h0, h1, h2\n\t" \
				 "fmov %w0, s0"                                            \
				 : "=r"(r)                                                 \
				 : "r"(std::uint32_t{a}), "r"(std::uint32_t{b})            \
				 : "v0", "v1", "v2");                                      \
		return static_cast<std::uint16_t>(r);                                      \
	}                                                                                  \
	[[nodiscard]] inline std::uint32_t name(std::uint32_t a, std::uint32_t b) noexcept \
	{                                                                                  \
		std::uint32_t r;                                                           \
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\t" insn " s0, s1, s2\n\t" \
				 "fmov %w0, s0"                                            \
				 : "=r"(r)                                                 \
				 : "r"(a), "r"(b)                                          \
				 : "v0", "v1", "v2");                                      \
		return r;                                                                  \
	}                                                                                  \
	[[nodiscard]] inline std::uint64_t name(std::uint64_t a, std::uint64_t b) noexcept \
	{                                                                                  \
		std::uint64_t r;                                                           \
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\t" insn " d0, d1, d2\n\t"   \
				 "fmov %0, d0"                                             \
				 : "=r"(r)                                                 \
				 : "r"(a), "r"(b)                                          \
				 : "v0", "v1", "v2");                                      \
		return r;                                                                  \
	}

#define UML_A32_FP_UNARY(name, insn)                                                  \
	[[nodiscard]] inline std::uint16_t name(std::uint16_t a) noexcept             \
	{                                                                             \
		std::uint32_t r;                                                      \
		__asm__ volatile("fmov s1, %w1\n\t" insn " h0, h1\n\tfmov %w0, s0"    \
				 : "=r"(r)                                            \
				 : "r"(std::uint32_t{a})                              \
				 : "v0", "v1");                                       \
		return static_cast<std::uint16_t>(r);                                 \
	}                                                                             \
	[[nodiscard]] inline std::uint32_t name(std::uint32_t a) noexcept             \
	{                                                                             \
		std::uint32_t r;                                                      \
		__asm__ volatile("fmov s1, %w1\n\t" insn " s0, s1\n\tfmov %w0, s0"    \
				 : "=r"(r)                                            \
				 : "r"(a)                                             \
				 : "v0", "v1");                                       \
		return r;                                                             \
	}                                                                             \
	[[nodiscard]] inline std::uint64_t name(std::uint64_t a) noexcept             \
	{                                                                             \
		std::uint64_t r;                                                      \
		__asm__ volatile("fmov d1, %1\n\t" insn " d0, d1\n\tfmov %0, d0"      \
				 : "=r"(r)                                            \
				 : "r"(a)                                             \
				 : "v0", "v1");                                       \
		return r;                                                             \
	}

UML_A32_FP_BINARY(add, "fadd")
UML_A32_FP_BINARY(sub, "fsub")
UML_A32_FP_BINARY(mul, "fmul")
UML_A32_FP_BINARY(div, "fdiv")
UML_A32_FP_BINARY(max, "fmax")
UML_A32_FP_BINARY(min, "fmin")
UML_A32_FP_BINARY(max_number, "fmaxnm")
UML_A32_FP_BINARY(min_number, "fminnm")
UML_A32_FP_BINARY(abd, "fabd")
/* The comparison masks: all ones when true. */
UML_A32_FP_BINARY(compare_eq, "fcmeq")
UML_A32_FP_BINARY(compare_ge, "fcmge")
UML_A32_FP_BINARY(compare_gt, "fcmgt")
UML_A32_FP_BINARY(abs_compare_ge, "facge")
UML_A32_FP_BINARY(abs_compare_gt, "facgt")

UML_A32_FP_UNARY(sqrt, "fsqrt")
UML_A32_FP_UNARY(recip_estimate, "frecpe")
UML_A32_FP_UNARY(rsqrt_estimate, "frsqrte")
/* Round to an integral value in the same format: to nearest with ties away
 * (a), to nearest even (n), toward plus infinity (p), toward minus infinity
 * (m), toward zero (z), by the FPCR (i), and by the FPCR raising Inexact (x). */
UML_A32_FP_UNARY(round_a, "frinta")
UML_A32_FP_UNARY(round_n, "frintn")
UML_A32_FP_UNARY(round_p, "frintp")
UML_A32_FP_UNARY(round_m, "frintm")
UML_A32_FP_UNARY(round_z, "frintz")
UML_A32_FP_UNARY(round_i, "frinti")
UML_A32_FP_UNARY(round_x, "frintx")

#undef UML_A32_FP_BINARY
#undef UML_A32_FP_UNARY

/* The layout of a format, for the steps and for the sign bit. */
template<typename Float>
struct Format;
template<>
struct Format<std::uint16_t> final {
	static constexpr unsigned fraction_bits = 10;
	static constexpr unsigned exponent_bits = 5;
	static constexpr std::uint16_t two = 0x4000;
	static constexpr std::uint16_t three = 0x4200;
	static constexpr std::uint16_t one_half = 0x3800;
};
template<>
struct Format<std::uint32_t> final {
	static constexpr unsigned fraction_bits = 23;
	static constexpr unsigned exponent_bits = 8;
	static constexpr std::uint32_t two = 0x4000'0000U;
	static constexpr std::uint32_t three = 0x4040'0000U;
	static constexpr std::uint32_t one_half = 0x3f00'0000U;
};

template<typename Float>
[[nodiscard]] constexpr std::uint32_t exponent_of(Float v) noexcept
{
	return static_cast<std::uint32_t>(v >> Format<Float>::fraction_bits) & ((1U << Format<Float>::exponent_bits) - 1U);
}
template<typename Float>
[[nodiscard]] constexpr std::uint32_t fraction_of(Float v) noexcept
{
	return static_cast<std::uint32_t>(v) & ((1U << Format<Float>::fraction_bits) - 1U);
}
template<typename Float>
[[nodiscard]] constexpr bool is_nan(Float v) noexcept
{
	return exponent_of(v) == (1U << Format<Float>::exponent_bits) - 1U && fraction_of(v) != 0U;
}
template<typename Float>
[[nodiscard]] constexpr bool is_infinity(Float v) noexcept
{
	return exponent_of(v) == (1U << Format<Float>::exponent_bits) - 1U && fraction_of(v) == 0U;
}
/* Whether FPUnpack under the Standard FPSCR value flushes a denormal of the
 * format to zero: FZ is always set, so single precision does, and half
 * precision follows the guest's FZ16. */
template<typename Float>
[[nodiscard]] constexpr bool unpack_flushes(std::uint32_t guest_fpcr) noexcept
{
	if constexpr (sizeof(Float) == sizeof(std::uint16_t))
		return (guest_fpcr & fpcr_fz16) != 0U;
	else
		return true;
}
/* Zero, or a denormal that FPUnpack flushes (`flush`). */
template<typename Float>
[[nodiscard]] constexpr bool is_flushed_zero(Float v, bool flush) noexcept
{
	return exponent_of(v) == 0U && (flush || fraction_of(v) == 0U);
}

/* VRECPS and VRSQRTS are not the fused A64 steps: AArch32 FPRecipStep and
 * FPRSqrtStep round the product first, treat infinity times zero (a
 * denormal is zero only where FPUnpack flushes it, `flush`: always for
 * single precision, FZ16 for half) as a zero product, and halve the RSqrt
 * result exactly. These run under the Standard FPSCR value. */
template<typename Float>
[[nodiscard]] inline Float step_product(Float a, Float b, bool flush, bool &nan) noexcept
{
	nan = is_nan(a) || is_nan(b);
	if (nan)
		return mul(a, b);
	if ((is_infinity(a) && is_flushed_zero(b, flush)) || (is_flushed_zero(a, flush) && is_infinity(b))) {
		/* FPUnpack flushes a denormal operand and a single-precision one sets IDC. */
		(void)add(a, Float{0});
		(void)add(b, Float{0});
		return Float{0};
	}
	return mul(a, b);
}
template<typename Float>
[[nodiscard]] inline Float recip_step(Float a, Float b, std::uint32_t guest_fpcr) noexcept
{
	bool nan;
	const Float product = step_product(a, b, unpack_flushes<Float>(guest_fpcr), nan);
	return nan ? product : sub(Format<Float>::two, product);
}
template<typename Float>
[[nodiscard]] inline Float rsqrt_step(Float a, Float b, std::uint32_t guest_fpcr) noexcept
{
	bool nan;
	const Float product = step_product(a, b, unpack_flushes<Float>(guest_fpcr), nan);
	return nan ? product : mul(sub(Format<Float>::three, product), Format<Float>::one_half);
}

/* The fused multiply-adds: addend + a * b (FMADD with the addend last). */
[[nodiscard]] inline std::uint16_t fused(std::uint16_t addend, std::uint16_t a,
	std::uint16_t b) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfmov s3, %w3\n\t"
			 "fmadd h0, h2, h3, h1\n\tfmov %w0, s0"
			 : "=r"(r)
			 : "r"(std::uint32_t{addend}), "r"(std::uint32_t{a}), "r"(std::uint32_t{b})
			 : "v0", "v1", "v2", "v3");
	return static_cast<std::uint16_t>(r);
}
[[nodiscard]] inline std::uint32_t fused(std::uint32_t addend, std::uint32_t a,
	std::uint32_t b) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfmov s3, %w3\n\t"
			 "fmadd s0, s2, s3, s1\n\tfmov %w0, s0"
			 : "=r"(r)
			 : "r"(addend), "r"(a), "r"(b)
			 : "v0", "v1", "v2", "v3");
	return r;
}
[[nodiscard]] inline std::uint64_t fused(std::uint64_t addend, std::uint64_t a,
	std::uint64_t b) noexcept
{
	std::uint64_t r;
	__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfmov d3, %3\n\t"
			 "fmadd d0, d2, d3, d1\n\tfmov %0, d0"
			 : "=r"(r)
			 : "r"(addend), "r"(a), "r"(b)
			 : "v0", "v1", "v2", "v3");
	return r;
}

/* FCMP / FCMPE: the NZCV result in bits 31:28. */
[[nodiscard]] inline std::uint32_t compare_flags(std::uint16_t a, std::uint16_t b,
	bool signal_quiet_nan) noexcept
{
	std::uint64_t flags;
	if (signal_quiet_nan)
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmpe h1, h2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(std::uint32_t{a}), "r"(std::uint32_t{b}) : "v1", "v2", "cc");
	else
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmp h1, h2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(std::uint32_t{a}), "r"(std::uint32_t{b}) : "v1", "v2", "cc");
	return static_cast<std::uint32_t>(flags) & 0xf000'0000U;
}
[[nodiscard]] inline std::uint32_t compare_flags(std::uint32_t a, std::uint32_t b,
	bool signal_quiet_nan) noexcept
{
	std::uint64_t flags;
	if (signal_quiet_nan)
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmpe s1, s2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	else
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmp s1, s2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	return static_cast<std::uint32_t>(flags) & 0xf000'0000U;
}
[[nodiscard]] inline std::uint32_t compare_flags(std::uint64_t a, std::uint64_t b,
	bool signal_quiet_nan) noexcept
{
	std::uint64_t flags;
	if (signal_quiet_nan)
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfcmpe d1, d2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	else
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfcmp d1, d2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	return static_cast<std::uint32_t>(flags) & 0xf000'0000U;
}

/* Selects the Standard FPSCR value for the lane helpers below. */
struct Standard final {};
inline constexpr Standard standard{};

/* A function of the n, m and old d elements of each D register of an
 * Advanced SIMD form (half or single precision by the form's esize), run
 * under the Standard FPSCR value. The function takes bit patterns of the
 * element type. */
template<typename Float, typename Function>
inline bool lanes_of(Exec &x, Function &&function)
{
	constexpr unsigned bits = sizeof(Float) * 8U;
	return run_standard(x.cpu, [&]() -> bool {
		return for_each_register(x, [&](std::uint64_t n, std::uint64_t m, std::uint64_t d) -> std::uint64_t {
			std::uint64_t out = 0;
			for (unsigned shift = 0; shift < 64U; shift += bits)
				out |= std::uint64_t{static_cast<Float>(function(static_cast<Float>(n >> shift),
					       static_cast<Float>(m >> shift),
					       static_cast<Float>(d >> shift)))}
					<< shift;
			return out;
		});
	});
}
template<typename Function>
inline bool lanes(Exec &x, Standard, Function &&function)
{
	if (x.o.esize == 16)
		return lanes_of<std::uint16_t>(x, function);
	return lanes_of<std::uint32_t>(x, function);
}
template<typename Float>
[[nodiscard]] constexpr Float sign_bit() noexcept
{
	return Float{1} << (sizeof(Float) * 8U - 1U);
}
template<typename Float>
[[nodiscard]] constexpr Float negated(Float value) noexcept
{
	return value ^ sign_bit<Float>();
}

/* A VFP operation on the H, S or D registers d, n and m: `function(d, n, m)`
 * is called with half, single or double bit patterns and returns the new d.
 * A half-precision result zero-extends into its S register. */
template<typename Function>
inline bool vfp_op(Exec &x, Function &&function)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned n = static_cast<unsigned>(o.n);
	const unsigned m = static_cast<unsigned>(o.m);
	if (o.esize == 64) {
		x.set_d_reg(d, fp::run_effective(x.cpu, [&] {
			return function(x.d_reg(d), x.d_reg(n), x.d_reg(m));
		}));
	} else if (o.esize == 16) {
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
			return std::uint32_t{function(static_cast<std::uint16_t>(x.s_reg(d)),
				static_cast<std::uint16_t>(x.s_reg(n)), static_cast<std::uint16_t>(x.s_reg(m)))};
		}));
	} else {
		x.set_s_reg(d, fp::run_effective(x.cpu, [&] {
			return function(x.s_reg(d), x.s_reg(n), x.s_reg(m));
		}));
	}
	return true;
}

/* The same function for a form that is Advanced SIMD (single-precision
 * elements under the Standard FPSCR value) or VFP, by the form's advsimd. */
template<typename Function>
inline bool float_op(Exec &x, Function &&function)
{
	if (x.o.advsimd)
		return fp::lanes(x, fp::standard, [&](auto n, auto m, auto d) { return function(d, n, m); });
	return vfp_op(x, function);
}

} // namespace uml::cpu::arm64::aarch32::detail::fp
