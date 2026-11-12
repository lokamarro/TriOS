// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/floating_rounding.hpp"
#include "cpu/floating_point_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace uml::cpu::arm64::detail {

/* AArch64 maps each FPCR exception-enable bit eight positions above the
 * matching cumulative FPSR bit.  Never install these guest enables in the
 * host FPCR: the emulator reports the guest trap after a transactional
 * instruction instead of allowing a host floating-point exception.
 */
inline constexpr std::uint32_t fpcr_exception_enable_mask = 0x0000'9f00U;
inline constexpr std::uint32_t fpsr_exception_cumulative_mask =
	::uml::cpu::floating_point_status_exception_mask;

/* The FPSR bits a natively executed operation reports: QC and the cumulative
 * exception flags including IDC. */
inline constexpr std::uint32_t fpsr_reported_mask = 0x0800'009fU;

[[nodiscard]] constexpr std::uint32_t host_fpcr(
	std::uint32_t guest_fpcr) noexcept
{
	return guest_fpcr & ~fpcr_exception_enable_mask;
}

template<typename Value>
struct LocalFpcrResult final {
	Value value;
	std::uint32_t fpsr;
};

/* Run operation() with local_fpcr installed and FPSR clear, then restore the
 * host FPCR and FPSR and return its value with the FPSR bits it raised. The
 * operation must issue its floating-point instructions as asm volatile so
 * that they stay between the control-register accesses. */
template<typename Operation>
[[nodiscard]] inline auto run_with_local_fpcr(std::uint64_t local_fpcr,
	Operation &&operation) noexcept
{
	std::uint64_t saved_fpcr;
	std::uint64_t saved_fpsr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr) : : "memory");
	__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr) : : "memory");
	__asm__ volatile("msr fpcr, %0" : : "r"(local_fpcr) : "memory");
	__asm__ volatile("msr fpsr, xzr" : : : "memory");
	const auto value = operation();
	std::uint64_t raised;
	__asm__ volatile("mrs %0, fpsr" : "=r"(raised) : : "memory");
	__asm__ volatile("msr fpcr, %0" : : "r"(saved_fpcr) : "memory");
	__asm__ volatile("msr fpsr, %0" : : "r"(saved_fpsr) : "memory");
	return LocalFpcrResult<std::remove_cv_t<decltype(value)>>{value,
		static_cast<std::uint32_t>(raised) & fpsr_reported_mask};
}

[[nodiscard]] constexpr std::uint32_t enabled_exception_flags(
	std::uint32_t guest_fpcr) noexcept
{
	return (guest_fpcr >> 8U) & fpsr_exception_cumulative_mask;
}

/* The 0b111 major opcode class is the register-only FP/Advanced-SIMD
 * data-processing space.  In particular this excludes every SIMD/FP memory
 * instruction, so an enabled guest FP trap never makes a memory operation
 * transactional.
 */
[[nodiscard]] constexpr bool is_fp_simd_data_processing(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0x0e00'0000U) == 0x0e00'0000U;
}

[[nodiscard]] constexpr bool needs_floating_exception_transaction(
	std::uint32_t guest_fpcr, std::uint32_t instruction) noexcept
{
	return (guest_fpcr & fpcr_exception_enable_mask) != 0U &&
		is_fp_simd_data_processing(instruction);
}

/* ESR_ELx: EC=0x2c (FP exception from AArch64), IL=1, TFV=1, and the
 * reported cause's FPSR flag in the low ISS bits.
 */
[[nodiscard]] constexpr std::uint32_t floating_exception_syndrome(
	std::uint32_t raised_enabled) noexcept
{
	return ::uml::cpu::floating_point_syndrome(raised_enabled);
}

struct FloatingExceptionDecision final {
	std::uint32_t newly_raised{};
	std::uint32_t raised_enabled{};
	std::uint32_t committed_fpsr{};
	std::uint32_t syndrome{};

	[[nodiscard]] constexpr bool traps() const noexcept
	{
		return raised_enabled != 0U;
	}
};

inline constexpr std::uint32_t fpsr_underflow = 1U << 3U;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7U;
inline constexpr std::uint32_t fpcr_underflow_enable = 1U << 11U;
inline constexpr std::uint32_t fpcr_flush_inputs_to_zero = 1U << 0U;
inline constexpr std::uint32_t fpcr_alternate_handling = 1U << 1U;
inline constexpr std::uint32_t fpcr_flush_to_zero_half = 1U << 19U;
inline constexpr std::uint32_t fpcr_flush_to_zero = 1U << 24U;

/* The one cause a trap reports when several enabled exceptions were raised:
 * Input Denormal, then Invalid Operation, Divide by Zero, Overflow,
 * Underflow and Inexact, whichever elements raised them. */
[[nodiscard]] constexpr std::uint32_t reported_floating_exception(
	std::uint32_t raised_enabled) noexcept
{
	if ((raised_enabled & fpsr_input_denormal) != 0U)
		return fpsr_input_denormal;
	return raised_enabled & (~raised_enabled + 1U);
}

/*
 * isolated_fpsr is the result of executing with every cumulative exception
 * flag clear and every trap enable clear; trappable is the part of its
 * cumulative flags that an enabled exception would trap, which for an
 * Underflow the instruction's rounding decides (trappable_floating_exceptions
 * below). When an enabled exception was raised the instruction traps,
 * reporting one cause and changing nothing, FPSR included; otherwise it
 * commits every flag it raised.
 */
[[nodiscard]] constexpr FloatingExceptionDecision
decide_floating_exception(std::uint32_t guest_fpcr,
	std::uint32_t original_fpsr, std::uint32_t isolated_fpsr,
	std::uint32_t trappable) noexcept
{
	const auto newly_raised =
		isolated_fpsr & fpsr_exception_cumulative_mask;
	const auto raised_enabled = trappable &
		fpsr_exception_cumulative_mask &
		enabled_exception_flags(guest_fpcr);
	if (raised_enabled != 0U) {
		const auto reported = reported_floating_exception(raised_enabled);
		return {
			newly_raised,
			reported,
			original_fpsr,
			floating_exception_syndrome(reported),
		};
	}
	return {newly_raised, 0U, original_fpsr | newly_raised, 0U};
}

/* A decision for a form whose every raised flag can trap. */
[[nodiscard]] constexpr FloatingExceptionDecision
decide_floating_exception(std::uint32_t guest_fpcr,
	std::uint32_t original_fpsr, std::uint32_t isolated_fpsr) noexcept
{
	return decide_floating_exception(
		guest_fpcr, original_fpsr, isolated_fpsr, isolated_fpsr);
}

/* Whether the flush-to-zero control of the form's result format flushes
 * a tiny result (FPRoundBase), under FPCR.AH as well. */
[[nodiscard]] constexpr bool flushes_tiny_result(
	std::uint32_t guest_fpcr, const FloatingRounding &rounding) noexcept
{
	if ((guest_fpcr & fpcr_alternate_handling) != 0U) {
		if (rounding.unflushed_under_ah)
			return false;
		if (rounding.flushed_under_ah)
			return true;
	}
	if (rounding.format == FloatingFormat::half)
		return !rounding.ignores_fz16 &&
			(guest_fpcr & fpcr_flush_to_zero_half) != 0U;
	return (guest_fpcr & fpcr_flush_to_zero) != 0U;
}

/* Whether an element in `format` is a nonzero subnormal number. */
[[nodiscard]] constexpr bool is_subnormal(
	std::uint64_t element, FloatingFormat format) noexcept
{
	switch (format) {
	case FloatingFormat::half:
		return (element & 0x7c00U) == 0U && (element & 0x03ffU) != 0U;
	case FloatingFormat::bfloat16:
		return (element & 0x7f80U) == 0U && (element & 0x007fU) != 0U;
	case FloatingFormat::single:
		return (element & 0x7f80'0000U) == 0U &&
			(element & 0x007f'ffffU) != 0U;
	case FloatingFormat::double_precision:
		return (element & 0x7ff0'0000'0000'0000ULL) == 0U &&
			(element & 0x000f'ffff'ffff'ffffULL) != 0U;
	case FloatingFormat::none:
		break;
	}
	return false;
}

namespace floating_reduction_detail {

enum class Kind : std::uint8_t {
	zero,
	subnormal,
	normal,
	infinity,
	quiet_nan,
	signalling_nan,
};

struct Element final {
	Kind kind{};
	bool sign{};
	/* The value's order: its magnitude bits, negated when negative. */
	std::int64_t key{};
	std::uint64_t bits{};
};

struct Format final {
	unsigned exponent_shift;
	std::uint64_t exponent_mask;
	std::uint64_t fraction_mask;
	std::uint64_t sign_bit;
	std::uint64_t quiet_bit;
};

[[nodiscard]] constexpr Format format_of(FloatingFormat format) noexcept
{
	if (format == FloatingFormat::half)
		return {10U, 0x1fU, 0x3ffU, 0x8000U, 0x200U};
	return {23U, 0xffU, 0x7f'ffffU, 0x8000'0000U, 0x40'0000U};
}

/* FPUnpack: a subnormal input that the flush controls flush is zero. */
[[nodiscard]] constexpr Element unpack(
	std::uint64_t bits, const Format &format, bool flush) noexcept
{
	const bool sign = (bits & format.sign_bit) != 0U;
	const std::uint64_t exponent =
		(bits >> format.exponent_shift) & format.exponent_mask;
	const std::uint64_t fraction = bits & format.fraction_mask;
	const auto magnitude =
		static_cast<std::int64_t>(bits & ~format.sign_bit);
	Element element{Kind::normal, sign, sign ? -magnitude : magnitude,
		bits};
	if (exponent == format.exponent_mask) {
		element.kind = fraction == 0U ? Kind::infinity :
			(fraction & format.quiet_bit) != 0U ?
						Kind::quiet_nan :
						Kind::signalling_nan;
	} else if (exponent == 0U) {
		element.kind = fraction != 0U && !flush ? Kind::subnormal :
							  Kind::zero;
		if (element.kind == Kind::zero)
			element.key = 0;
	}
	return element;
}

[[nodiscard]] constexpr bool is_nan(const Element &element) noexcept
{
	return element.kind == Kind::quiet_nan ||
		element.kind == Kind::signalling_nan;
}

struct Step final {
	std::uint64_t bits;
	bool underflow;
};

/*
 * FPMax or FPMin of two elements: the result's bits, and whether it passed
 * a tiny value to FPRound unflushed. With `alternate`, the FEAT_AFP
 * handling of zeros and NaNs applies and the result is never flushed.
 */
[[nodiscard]] constexpr Step maximum_or_minimum(Element first,
	Element second, bool maximum, bool alternate, bool flush_result,
	const Format &format) noexcept
{
	const auto zero = [&](bool sign) {
		return Step{sign ? format.sign_bit : 0U, false};
	};
	if (alternate && first.kind == Kind::zero &&
		second.kind == Kind::zero && first.sign != second.sign)
		return zero(second.sign);
	if (alternate && (is_nan(first) || is_nan(second)))
		return second.kind == Kind::zero ? zero(second.sign) :
						   Step{second.bits, false};
	if (is_nan(first) || is_nan(second))
		return {(format.exponent_mask << format.exponent_shift) |
				format.quiet_bit,
			false};
	const bool pick_first =
		maximum ? first.key > second.key : first.key < second.key;
	const Element &picked = pick_first ? first : second;
	if (picked.kind == Kind::infinity)
		return {picked.bits, false};
	if (picked.kind == Kind::zero)
		return zero(maximum ? (first.sign && second.sign) :
				      (first.sign || second.sign));
	if (picked.kind == Kind::subnormal)
		return flush_result ? zero(picked.sign) :
				      Step{picked.bits, true};
	return {picked.bits, false};
}

/* Whether FPReduce's tree of FMAX, FMIN, FMAXNM or FMINNM steps over the
 * low `bytes` of `source` rounds a tiny value unflushed. */
[[nodiscard]] constexpr bool reduction_underflows(std::uint32_t guest_fpcr,
	const FloatingRounding &rounding,
	const std::array<std::byte, 16> &source) noexcept
{
	const Format format = format_of(rounding.format);
	const unsigned size = rounding.format == FloatingFormat::half ? 2U : 4U;
	const bool alternate = (guest_fpcr & fpcr_alternate_handling) != 0U;
	const bool half = rounding.format == FloatingFormat::half;
	const bool flush_input = half ?
		(guest_fpcr & fpcr_flush_to_zero_half) != 0U :
		(guest_fpcr & fpcr_flush_inputs_to_zero) != 0U ||
			((guest_fpcr & fpcr_flush_to_zero) != 0U && !alternate);
	const bool flush_output = (guest_fpcr &
		(half ? fpcr_flush_to_zero_half : fpcr_flush_to_zero)) != 0U;
	const bool maximum =
		rounding.reduction == FloatingReduction::maximum ||
		rounding.reduction == FloatingReduction::maximum_number;
	const bool number =
		rounding.reduction == FloatingReduction::maximum_number ||
		rounding.reduction == FloatingReduction::minimum_number;
	std::array<std::uint64_t, 8> level{};
	unsigned count = rounding.reduced_bytes / size;
	for (unsigned index = 0; index < count; ++index)
		for (unsigned byte = 0; byte < size; ++byte)
			level[index] |= std::uint64_t{std::to_integer<std::uint8_t>(
						source[index * size + byte])}
				<< (8U * byte);
	bool underflow = false;
	for (; count > 1U; count /= 2U) {
		for (unsigned index = 0; index < count / 2U; ++index) {
			Element low = unpack(level[2U * index], format,
				flush_input);
			Element high = unpack(level[2U * index + 1U], format,
				flush_input);
			bool step_alternate = alternate;
			bool step_flush = flush_output && !alternate;
			if (number) {
				/* FPMaxNum and FPMinNum treat a single quiet
				 * NaN as the infinity that loses, and use
				 * FPMax and FPMin without the alternate
				 * handling. */
				if (!(alternate && is_nan(low) && is_nan(high))) {
					const std::uint64_t loses =
						(format.exponent_mask
							<< format.exponent_shift) |
						(maximum ? format.sign_bit : 0U);
					if (low.kind == Kind::quiet_nan &&
						high.kind != Kind::quiet_nan)
						low = unpack(loses, format, false);
					else if (low.kind != Kind::quiet_nan &&
						high.kind == Kind::quiet_nan)
						high = unpack(loses, format, false);
				}
				step_alternate = false;
				step_flush = flush_output;
			}
			const Step step = maximum_or_minimum(low, high, maximum,
				step_alternate, step_flush, format);
			level[index] = step.bits;
			underflow = underflow || step.underflow;
		}
	}
	return underflow;
}

} // namespace floating_reduction_detail

/*
 * The flags of `raised` that trap while FPCR.UFE is set, given the form's
 * rounding, its source Vn as it was before the instruction, and its result
 * in Vd (the isolated execution's). A flush to
 * zero sets FPSR.UFC but never traps, and a rounded result element that is
 * subnormal was tiny and so raises Underflow even when it is exact
 * (FPRoundBase's trapped_UF), which an execution with UFE clear does not
 * report.
 */
[[nodiscard]] constexpr std::uint32_t trappable_floating_exceptions(
	std::uint32_t guest_fpcr, std::uint32_t raised,
	const FloatingRounding &rounding,
	const std::array<std::byte, 16> &source,
	const std::array<std::byte, 16> &destination) noexcept
{
	if ((guest_fpcr & fpcr_underflow_enable) == 0U ||
		rounding.kind == FloatingResultKind::none)
		return raised;
	if (rounding.kind == FloatingResultKind::estimate ||
		flushes_tiny_result(guest_fpcr, rounding))
		return raised & ~fpsr_underflow;
	if ((raised & fpsr_underflow) != 0U)
		return raised;
	if (rounding.reduction != FloatingReduction::none)
		return floating_reduction_detail::reduction_underflows(
			       guest_fpcr, rounding, source) ?
			raised | fpsr_underflow :
			raised;
	const unsigned size = floating_rounding_detail::format_bytes(
		rounding.format);
	for (unsigned offset = rounding.first_byte;
		offset + size <= rounding.first_byte + rounding.bytes;
		offset += size) {
		std::uint64_t element = 0;
		for (unsigned byte = 0; byte < size; ++byte)
			element |= std::uint64_t{std::to_integer<std::uint8_t>(
					   destination[offset + byte])}
				<< (8U * byte);
		if (is_subnormal(element, rounding.format))
			return raised | fpsr_underflow;
	}
	return raised;
}

/*
 * The same rules for a form that notes itself whether it rounded a tiny
 * value without flushing it, a result or an intermediate result.
 */
[[nodiscard]] constexpr std::uint32_t trappable_floating_exceptions(
	std::uint32_t guest_fpcr, std::uint32_t raised,
	const FloatingRounding &rounding, bool tiny) noexcept
{
	if ((guest_fpcr & fpcr_underflow_enable) == 0U ||
		rounding.kind == FloatingResultKind::none)
		return raised;
	if (rounding.kind == FloatingResultKind::estimate ||
		flushes_tiny_result(guest_fpcr, rounding))
		return raised & ~fpsr_underflow;
	return tiny ? raised | fpsr_underflow : raised;
}

/* The decision for an A64 FP&SIMD data-processing instruction, from Vn
 * before it and the isolated execution's FPSR and Vd. */
[[nodiscard]] constexpr FloatingExceptionDecision
decide_floating_exception(std::uint32_t guest_fpcr,
	std::uint32_t original_fpsr, std::uint32_t isolated_fpsr,
	std::uint32_t instruction, const std::array<std::byte, 16> &source,
	const std::array<std::byte, 16> &destination) noexcept
{
	return decide_floating_exception(guest_fpcr, original_fpsr,
		isolated_fpsr,
		trappable_floating_exceptions(guest_fpcr, isolated_fpsr,
			classify_floating_rounding(instruction), source,
			destination));
}

/* The same decision, compiled once for both engines' trap paths. */
[[nodiscard]] FloatingExceptionDecision decide_a64_floating_exception(
	std::uint32_t guest_fpcr, std::uint32_t original_fpsr,
	std::uint32_t isolated_fpsr, std::uint32_t instruction,
	const std::array<std::byte, 16> &source,
	const std::array<std::byte, 16> &destination) noexcept;

} // namespace uml::cpu::arm64::detail
