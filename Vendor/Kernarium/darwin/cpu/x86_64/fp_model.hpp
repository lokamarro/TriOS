// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* x86's SIMD floating-point results and MXCSR flags, where the host's
 * arithmetic differs. The JIT's cold path of FEXCore's EmbedderFPCheck op
 * hands an operation's x86 sources and the host's result here; the gadget
 * engine's checked run takes only the flags, from the sources of the
 * instruction it runs, and keeps its own results. The model computes in
 * integers and SoftFloat only, because the JIT runs it inside a dispatch,
 * under the guest's FPCR. */

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

/* The x86 operation of one instruction, or of one 128-bit part of it, as
 * FEXCore's EmbedderFPKind names it. */
enum class FpKind : std::uint8_t {
	add,
	sub,
	mul,
	div,
	min,
	max,
	/* SQRTPS to SQRTSD. The host's root of a positive normal lane is x86's,
	 * so the model leaves that lane's result and decides only precision;
	 * it computes the others. */
	sqrt,
	rcp,
	rsqrt,
	/* HADDPS and HADDPD, HSUBPS and HSUBPD: pairs of the first source,
	 * then of the second. */
	hadd,
	hsub,
	/* ADDSUBPS and ADDSUBPD: even lanes subtract. */
	addsub,
	/* DPPS and DPPD, with the instruction's immediate. */
	dpp,
	/* a * b + c, with the signs `flags` gives. */
	fma,
	/* CVTSD2SS and CVTPD2PS: the doubles of the first source, then of the
	 * second, to singles. The element is the double, the lanes are the
	 * singles. */
	narrow,
	/* The kinds below raise exceptions only: the host's result is x86's,
	 * and the model leaves `result` alone. Lanes past the first source's
	 * 16 bytes are the second source's. */
	/* CMPPS to CMPSD and VCMP, with the predicate as the immediate. */
	cmp,
	/* COMISS and COMISD, or with `fp_comis_quiet` UCOMISS and UCOMISD. */
	comis,
	/* Floats to integers. */
	to_int,
	/* CVTSS2SD and CVTPS2PD. */
	widen,
	/* Integers to floats of the element. */
	from_int,
	/* ROUNDPS to ROUNDSD, with the immediate. */
	round,
	/* VCVTPH2PS; the element is the half. */
	half_widen,
	/* VCVTPS2PH, with the immediate. */
	half_narrow,
};

/* An FMA's signs: the product negated, and the addend negated in every
 * lane, the even lanes or the odd ones. */
constexpr std::uint8_t fp_fma_negate_product = 1U << 0U;
constexpr std::uint8_t fp_fma_negate_addend = 1U << 1U;
constexpr std::uint8_t fp_fma_negate_even_addends = 2U << 1U;
constexpr std::uint8_t fp_fma_negate_odd_addends = 3U << 1U;
/* A VEX comparison's five-bit predicate. */
constexpr std::uint8_t fp_cmp_vex = 1U << 0U;
constexpr std::uint8_t fp_comis_quiet = 1U << 0U;
/* A conversion to integers that truncates, and one to 64-bit integers. */
constexpr std::uint8_t fp_to_int_truncate = 1U << 0U;
constexpr std::uint8_t fp_to_int64 = 1U << 1U;
/* A conversion from 64-bit integers. */
constexpr std::uint8_t fp_from_int64 = 1U << 0U;

struct FpOperation final {
	FpKind kind{};
	/* Of each lane, or of a conversion's source lane: 4 or 8. */
	std::uint32_t element_bytes{};
	/* The lanes the operation computes, from lane 0. */
	std::uint32_t lanes{};
	std::uint8_t immediate{};
	std::uint8_t flags{};
};

using FpVector = std::array<std::byte, 16>;

struct FpValues final {
	/* The sources in x86's order; an FMA's are a, b and c of a * b + c,
	 * and a unary operation's is the first. */
	std::array<FpVector, 3> sources{};
	/* The host's result on entry, x86's on return: lanes the operation does
	 * not compute keep their value. */
	FpVector result{};
};

/* The MXCSR exception flags an operation raises, by step: x86 detects the
 * invalid-operation, denormal-operand and division-by-zero exceptions of a
 * step before computing it and the others after, and a DPPS or DPPD
 * computes in three steps. */
struct FpSteps final {
	static constexpr std::size_t count = 3;
	std::array<std::uint32_t, count> before{};
	std::array<std::uint32_t, count> after{};

	FpSteps &operator|=(const FpSteps &other) noexcept;
};

/* What an instruction does to MXCSR: the flags it raises, and whether it
 * faults with #XM instead of writing its destination. */
struct FpOutcome final {
	std::uint32_t flags{};
	bool fault{};
};

/* Replaces the computed lanes of `values.result` with x86's results under
 * `mxcsr`, and returns the flags it raises. */
FpSteps x86_fp_compute(const FpOperation &operation, std::uint32_t mxcsr,
	FpValues &values) noexcept;

/* An instruction's outcome from the flags of all its parts: x86 faults at
 * the first step whose flags MXCSR unmasks, before or after computing it,
 * and raises the flags up to that point. */
FpOutcome x86_fp_settle(const FpSteps &steps, std::uint32_t mxcsr) noexcept;

} // namespace uml::cpu::x86_64
