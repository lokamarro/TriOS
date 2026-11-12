// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/floating_exception.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

/*
 * Both engines decide guest floating-point traps through
 * detail/floating_exception.hpp. This test pins that shared contract, which
 * follows the M1's own trapping; the engines' trap behaviour is covered by
 * the corpus's trap records, arm64-floating-trap-control and
 * arm64-deferred-fp-state.
 */

namespace detail = uml::cpu::arm64::detail;

namespace {

constexpr std::uint32_t invalid = 1U << 0U;
constexpr std::uint32_t divide_by_zero = 1U << 1U;
constexpr std::uint32_t overflow = 1U << 2U;
constexpr std::uint32_t underflow = 1U << 3U;
constexpr std::uint32_t inexact = 1U << 4U;
constexpr std::uint32_t input_denormal = 1U << 7U;
constexpr std::uint32_t saturation = 1U << 27U;

constexpr std::uint32_t enable(std::uint32_t flags)
{
	return flags << 8U;
}

constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
constexpr std::uint32_t ldr_q0_x0 = 0x3d40'0000U;
constexpr std::uint32_t add_x10_x10_1 = 0x9100'054aU;
/* size==00 is reserved in this Advanced-SIMD data-processing class. */
constexpr std::uint32_t reserved_simd = 0x2e00'fc00U;

/* The host FPCR never carries a guest trap enable. */
static_assert(detail::host_fpcr(0xffff'ffffU) ==
	(0xffff'ffffU & ~detail::fpcr_exception_enable_mask));
static_assert(detail::enabled_exception_flags(0x0000'9f00U) == 0x9fU);
static_assert(detail::enabled_exception_flags(enable(overflow)) == overflow);

/*
 * Only the register-only FP/SIMD data-processing space is transactional.
 * A reserved encoding in that space stays in it: the engines reject it
 * before execution, so enables never turn it into a trap.
 */
static_assert(detail::is_fp_simd_data_processing(fdiv_s0_s1_s2));
static_assert(detail::is_fp_simd_data_processing(reserved_simd));
static_assert(!detail::is_fp_simd_data_processing(ldr_q0_x0));
static_assert(!detail::is_fp_simd_data_processing(add_x10_x10_1));
static_assert(detail::needs_floating_exception_transaction(
	enable(divide_by_zero), fdiv_s0_s1_s2));
static_assert(!detail::needs_floating_exception_transaction(
	0U, fdiv_s0_s1_s2));
static_assert(!detail::needs_floating_exception_transaction(
	detail::fpcr_exception_enable_mask, ldr_q0_x0));
static_assert(!detail::needs_floating_exception_transaction(
	detail::fpcr_exception_enable_mask, add_x10_x10_1));

/* ESR_ELx: EC 0x2c, IL, TFV, then the raised enabled cause flags. */
static_assert(detail::floating_exception_syndrome(divide_by_zero) ==
	((0x2cU << 26U) | (1U << 25U) | (1U << 23U) | divide_by_zero));
static_assert(detail::floating_exception_syndrome(saturation | overflow) ==
	detail::floating_exception_syndrome(overflow));

/* FDIV 1/0 with DZE: the enabled cause traps and FPSR stays unchanged. */
constexpr auto divide_trap = detail::decide_floating_exception(
	enable(divide_by_zero), saturation | input_denormal | invalid,
	divide_by_zero);
static_assert(divide_trap.traps());
static_assert(divide_trap.newly_raised == divide_by_zero);
static_assert(divide_trap.raised_enabled == divide_by_zero);
static_assert(divide_trap.committed_fpsr ==
	(saturation | input_denormal | invalid));
static_assert(divide_trap.syndrome ==
	detail::floating_exception_syndrome(divide_by_zero));

/*
 * FMUL overflow with only OFE: the overflow traps, and the trap commits
 * nothing, not even the unenabled inexact flag the instruction raised.
 */
constexpr auto overflow_trap = detail::decide_floating_exception(
	enable(overflow), input_denormal, overflow | inexact);
static_assert(overflow_trap.traps());
static_assert(overflow_trap.raised_enabled == overflow);
static_assert(overflow_trap.committed_fpsr == input_denormal);
static_assert(overflow_trap.syndrome ==
	detail::floating_exception_syndrome(overflow));

/* Several enabled causes report one, by priority: Input Denormal, then
 * Invalid Operation, Divide by Zero, Overflow, Underflow and Inexact. */
static_assert(detail::reported_floating_exception(0x9fU) == input_denormal);
static_assert(detail::reported_floating_exception(
		      inexact | divide_by_zero | invalid) == invalid);
static_assert(detail::reported_floating_exception(
		      inexact | overflow | divide_by_zero) == divide_by_zero);
static_assert(detail::reported_floating_exception(overflow | underflow) ==
	overflow);
static_assert(detail::reported_floating_exception(inexact | underflow) ==
	underflow);
constexpr auto lane_causes = detail::decide_floating_exception(
	enable(invalid | divide_by_zero | inexact), saturation,
	invalid | divide_by_zero | inexact);
static_assert(lane_causes.syndrome ==
	detail::floating_exception_syndrome(invalid));
static_assert(lane_causes.committed_fpsr == saturation);

/* FDIV 1/0 with only IOE: an unenabled cause commits normally. */
constexpr auto divide_commit = detail::decide_floating_exception(
	enable(invalid), input_denormal, divide_by_zero);
static_assert(!divide_commit.traps());
static_assert(divide_commit.raised_enabled == 0U);
static_assert(divide_commit.committed_fpsr ==
	(input_denormal | divide_by_zero));

/*
 * The isolated FPSR starts with every cumulative flag clear, so a flag that
 * was already set does not trap again unless the instruction raises it.
 */
constexpr auto already_set = detail::decide_floating_exception(
	enable(divide_by_zero), divide_by_zero, 0U);
static_assert(!already_set.traps());
static_assert(already_set.committed_fpsr == divide_by_zero);

/* Only the cumulative exception bits of the isolated FPSR can trap. */
constexpr auto saturation_only = detail::decide_floating_exception(
	detail::fpcr_exception_enable_mask, 0U, saturation);
static_assert(!saturation_only.traps());
static_assert(saturation_only.newly_raised == 0U);

/*
 * Underflow while FPCR.UFE is set (FPRoundBase). FMUL S0, S1, S2 of
 * 2^-65 by 2^-65 is 2^-130 exactly, a subnormal its execution with UFE
 * clear does not flag; under FZ the same product flushes to zero with only
 * FPSR.UFC, which never traps.
 */
constexpr std::uint32_t fmul_s0_s1_s2 = 0x1e22'0820U;
constexpr std::uint32_t fabs_s0_s1 = 0x1e20'c020U;
constexpr std::uint32_t fcvt_h0_s1 = 0x1e23'c020U;
constexpr std::uint32_t frecpe_s0_s1 = 0x5ea1'd820U;
constexpr std::uint32_t fcvtn2_v0_8h_v1_4s = 0x4e21'6820U;
constexpr std::uint32_t fmaxv_s0_v1_4s = 0x6e30'f820U;
constexpr std::uint32_t fz = 1U << 24U;
constexpr std::uint32_t fz16 = 1U << 19U;
constexpr std::array<std::byte, 16> zero_vector{};
constexpr std::array<std::byte, 16> exact_tiny{
	std::byte{0x00}, std::byte{0x00}, std::byte{0x08}, std::byte{0x00}};

static_assert(detail::classify_floating_rounding(fmul_s0_s1_s2).kind ==
	detail::FloatingResultKind::rounded);
static_assert(detail::classify_floating_rounding(fabs_s0_s1).kind ==
	detail::FloatingResultKind::none);
static_assert(detail::classify_floating_rounding(frecpe_s0_s1).kind ==
	detail::FloatingResultKind::estimate);
static_assert(detail::classify_floating_rounding(fcvt_h0_s1).ignores_fz16);
static_assert(detail::classify_floating_rounding(fcvtn2_v0_8h_v1_4s)
		      .first_byte == 8U);
static_assert(detail::classify_floating_rounding(fmaxv_s0_v1_4s).reduction ==
	detail::FloatingReduction::maximum);

constexpr auto exact_tiny_trap = detail::decide_floating_exception(
	enable(underflow), 0U, 0U, fmul_s0_s1_s2, zero_vector, exact_tiny);
static_assert(exact_tiny_trap.traps());
static_assert(exact_tiny_trap.syndrome ==
	detail::floating_exception_syndrome(underflow));
static_assert(!detail::decide_floating_exception(
	enable(inexact), 0U, 0U, fmul_s0_s1_s2, zero_vector, exact_tiny)
		       .traps());
static_assert(!detail::decide_floating_exception(
	enable(underflow), 0U, 0U, fabs_s0_s1, zero_vector, exact_tiny)
		       .traps());

constexpr auto flushed = detail::decide_floating_exception(
	enable(underflow) | fz, saturation, underflow, fmul_s0_s1_s2,
	zero_vector, zero_vector);
static_assert(!flushed.traps());
static_assert(flushed.committed_fpsr == (saturation | underflow));
/* FZ16 flushes a half-precision product but not FCVT's, which ignores it. */
static_assert(detail::decide_floating_exception(enable(underflow) | fz16,
	0U, underflow | inexact, fcvt_h0_s1, zero_vector, zero_vector)
		      .traps());
/* FRECPE's only Underflow is its flush to zero. */
static_assert(!detail::decide_floating_exception(enable(underflow) | fz,
	0U, underflow, frecpe_s0_s1, zero_vector, zero_vector)
		       .traps());
/* A tiny result that rounds inexactly traps as before. */
static_assert(detail::decide_floating_exception(enable(underflow), 0U,
	underflow | inexact, fmul_s0_s1_s2, zero_vector, exact_tiny)
		      .traps());

/* FMAXV rounds every step: max(2^-130, 2^-131) is tiny although the
 * final maximum, 1.0, is not. */
constexpr std::array<std::byte, 16> fmaxv_source{std::byte{0x00},
	std::byte{0x00}, std::byte{0x08}, std::byte{0x00}, std::byte{0x00},
	std::byte{0x00}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
	std::byte{0x00}, std::byte{0x80}, std::byte{0x3f}, std::byte{0x00},
	std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 16> one{std::byte{0x00}, std::byte{0x00},
	std::byte{0x80}, std::byte{0x3f}};
static_assert(detail::decide_floating_exception(enable(underflow), 0U, 0U,
	fmaxv_s0_v1_4s, fmaxv_source, one)
		      .traps());
static_assert(!detail::decide_floating_exception(enable(underflow), 0U, 0U,
	fmaxv_s0_v1_4s, one, one)
		       .traps());

} // namespace

int main()
{
	std::puts("arm64-floating-exception: shared trap contract holds");
	return 0;
}
