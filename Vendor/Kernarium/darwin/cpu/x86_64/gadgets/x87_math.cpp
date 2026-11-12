// SPDX-License-Identifier: GPL-2.0-or-later
#include "x87_math.hpp"

#include <SoftFloat-3e/platform.h>
#include <cephes_128bit.h>

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint16_t status_invalid = 1U << 0U;
constexpr std::uint16_t status_denormal = 1U << 1U;
constexpr std::uint16_t status_zero_divide = 1U << 2U;
constexpr std::uint16_t status_overflow = 1U << 3U;
constexpr std::uint16_t status_underflow = 1U << 4U;
constexpr std::uint16_t status_precision = 1U << 5U;
constexpr std::uint16_t status_error_summary = 1U << 7U;
constexpr std::uint16_t status_exception_mask = 0x7fU;
/* The six exceptions the control word masks. */
constexpr std::uint16_t status_maskable_exceptions = 0x3fU;
constexpr std::uint16_t status_c1 = 1U << 9U;
/* C0, C1, C2 and C3. */
constexpr std::uint16_t status_condition_codes = 0x4700U;
constexpr std::uint16_t status_c2 = 1U << 10U;
/* The denormal-operand exception, which SoftFloat has no flag for, in a
 * bit of softfloat_state::exceptionFlags that SoftFloat does not use. */
constexpr std::uint_fast8_t flag_denormal = 0x20U;

[[nodiscard]] constexpr extFloat80_t to_soft(
	const X87Register &value) noexcept
{
	return {value.significand, value.sign_exponent};
}

[[nodiscard]] constexpr X87Register from_soft(
	extFloat80_t value) noexcept
{
	return {value.signif, value.signExp};
}

[[nodiscard]] softfloat_state soft_state(const CpuState &cpu,
	bool force_80_bit_precision = false) noexcept
{
	softfloat_state state{};
	state.detectTininess = softfloat_tininess_afterRounding;
	switch ((cpu.x87_control >> 10U) & 3U) {
	case 1: state.roundingMode = softfloat_round_min; break;
	case 2: state.roundingMode = softfloat_round_max; break;
	case 3: state.roundingMode = softfloat_round_minMag; break;
	default: state.roundingMode = softfloat_round_near_even; break;
	}
	if (force_80_bit_precision) {
		state.roundingPrecision = 80;
	} else {
		switch ((cpu.x87_control >> 8U) & 3U) {
		case 0: state.roundingPrecision = 32; break;
		case 2: state.roundingPrecision = 64; break;
		default: state.roundingPrecision = 80; break;
		}
	}
	/* While the control word unmasks overflow or underflow, an extended
	 * result out of range is rebiased, as x87 delivers it to a register;
	 * the runtime's checked run then commits it. */
	if ((cpu.x87_control & status_overflow) == 0)
		state.rebias |= softfloat_rebias_overflow;
	if ((cpu.x87_control & status_underflow) == 0)
		state.rebias |= softfloat_rebias_underflow;
	return state;
}

void commit_exceptions(CpuState &cpu,
	const softfloat_state &state) noexcept
{
	std::uint16_t raised{};
	/* An invalid operation takes precedence over a denormal operand. */
	if ((state.exceptionFlags & softfloat_flag_invalid) != 0)
		raised |= status_invalid;
	else if ((state.exceptionFlags & flag_denormal) != 0)
		raised |= status_denormal;
	if ((state.exceptionFlags & softfloat_flag_infinite) != 0)
		raised |= status_zero_divide;
	if ((state.exceptionFlags & softfloat_flag_overflow) != 0)
		raised |= status_overflow;
	if ((state.exceptionFlags & softfloat_flag_underflow) != 0)
		raised |= status_underflow;
	if ((state.exceptionFlags & softfloat_flag_inexact) != 0)
		raised |= status_precision;
	/* While the gadgets check x87 exceptions, one the guest unmasked
	 * leaves the status word alone: the gadget leaves before it commits
	 * the result, and the runtime runs the instruction again, checked. */
	if ((cpu.exception_checks & exception_check_x87) != 0 &&
		(raised & ~cpu.x87_control & status_maskable_exceptions) != 0) {
		cpu.exception_raised = 1;
		return;
	}
	cpu.x87_status |= raised;
	/* C1 says whether the result was rounded up, away from zero; an
	 * invalid operation leaves no rounded result. */
	cpu.x87_status &= static_cast<std::uint16_t>(~status_c1);
	if ((state.exceptionFlags & softfloat_flag_roundedUp) != 0 &&
		(raised & status_invalid) == 0)
		cpu.x87_status |= status_c1;
	const auto pending = static_cast<std::uint16_t>(
		cpu.x87_status & status_exception_mask & ~cpu.x87_control);
	if (pending != 0)
		cpu.x87_status |= status_error_summary;
}

/* Raises the denormal-operand exception for an operand that is denormal or
 * pseudo-denormal. */
void denormal_operand(softfloat_state &state,
	const X87Register &value) noexcept
{
	if ((value.sign_exponent & 0x7fffU) == 0 && value.significand != 0)
		state.exceptionFlags |= flag_denormal;
}

[[nodiscard]] X87Register from_float32(CpuState &cpu,
	std::uint32_t bits) noexcept
{
	auto state = soft_state(cpu);
	if ((bits & 0x7f800000U) == 0 && (bits & 0x007fffffU) != 0)
		state.exceptionFlags |= flag_denormal;
	const auto result = f32_to_extF80(&state, float32_t{bits});
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register from_float64(CpuState &cpu,
	std::uint64_t bits) noexcept
{
	auto state = soft_state(cpu);
	if ((bits & 0x7ff0000000000000ULL) == 0 &&
		(bits & 0x000fffffffffffffULL) != 0)
		state.exceptionFlags |= flag_denormal;
	const auto result = f64_to_extF80(&state, float64_t{bits});
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register from_i32(std::int32_t value) noexcept
{
	return from_soft(i32_to_extF80(value));
}

[[nodiscard]] X87Register from_i64(std::int64_t value) noexcept
{
	const bool negative = value < 0;
	const auto magnitude = negative ?
		std::uint64_t{0} - static_cast<std::uint64_t>(value) :
		static_cast<std::uint64_t>(value);
	auto result = from_soft(ui64_to_extF80(magnitude));
	if (negative)
		result.sign_exponent |= 0x8000U;
	return result;
}

[[nodiscard]] std::uint32_t to_float32(
	CpuState &cpu, X87Register value) noexcept
{
	auto state = soft_state(cpu);
	const auto result = extF80_to_f32(&state, to_soft(value));
	commit_exceptions(cpu, state);
	return result.v;
}

[[nodiscard]] std::uint64_t to_float64(
	CpuState &cpu, X87Register value) noexcept
{
	auto state = soft_state(cpu);
	const auto result = extF80_to_f64(&state, to_soft(value));
	commit_exceptions(cpu, state);
	return result.v;
}

[[nodiscard]] constexpr bool is_zero(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0 && value.significand == 0;
}

[[nodiscard]] constexpr bool is_infinity(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0x7fffU &&
		value.significand == 0x8000000000000000ULL;
}

[[nodiscard]] constexpr bool is_nan(const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) == 0x7fffU &&
		(value.significand & 0x7fffffffffffffffULL) != 0;
}

[[nodiscard]] constexpr bool is_signaling(const X87Register &value) noexcept
{
	return is_nan(value) && (value.significand & 0x4000000000000000ULL) == 0;
}

[[nodiscard]] constexpr X87Register quiet_nan() noexcept
{
	return {0xc000000000000000ULL, 0x7fffU};
}

/* A pseudo-infinity, pseudo-NaN or unnormal, which x86 does not accept as an
 * operand. */
[[nodiscard]] constexpr bool is_unsupported(
	const X87Register &value) noexcept
{
	return (value.sign_exponent & 0x7fffU) != 0 &&
		(value.significand >> 63U) == 0;
}

[[nodiscard]] float128_t to_float128(softfloat_state &state,
	X87Register value) noexcept
{
	/* A pseudo-denormal has the value its significand has with exponent 1. */
	if ((value.sign_exponent & 0x7fffU) == 0 &&
		(value.significand >> 63U) != 0)
		value.sign_exponent |= 1U;
	return extF80_to_f128(&state, to_soft(value));
}

[[nodiscard]] X87Register from_float128(softfloat_state &state,
	float128_t value) noexcept
{
	return from_soft(f128_to_extF80(&state, value));
}

/* Which transcendental results are inexact, though a result's 128-bit
 * value may round to the operand itself or to 1, as for a tiny operand. */
enum class Inexact : std::uint8_t {
	never,
	always,
	/* An arctangent: any but a zero, an infinity or a NaN. */
	unless_zero,
};

/* Rounds a transcendental result to 80 bits, raising precision for it as
 * `inexact` says, and underflow beside it for a denormal result, as x86
 * does, then commits the exceptions. */
[[nodiscard]] X87Register inexact_result(CpuState &cpu,
	softfloat_state &state, float128_t value, Inexact inexact) noexcept
{
	const auto before = state.exceptionFlags;
	state.exceptionFlags = static_cast<std::uint8_t>(
		state.exceptionFlags & ~softfloat_flag_inexact);
	const auto converted = from_float128(state, value);
	const bool raise = inexact == Inexact::always ||
		(inexact == Inexact::unless_zero && !is_zero(converted) &&
			!is_nan(converted) && !is_infinity(converted));
	if (raise && (state.exceptionFlags & softfloat_flag_inexact) == 0) {
		state.exceptionFlags |= softfloat_flag_inexact;
		if ((converted.sign_exponent & 0x7fffU) == 0 &&
			converted.significand != 0)
			state.exceptionFlags |= softfloat_flag_underflow;
	}
	state.exceptionFlags |= before;
	commit_exceptions(cpu, state);
	return converted;
}

enum class TranscendentalOperation : std::uint8_t {
	sine,
	cosine,
	tangent,
};

/* FSIN, FCOS, FPTAN and FSINCOS reduce an operand of magnitude below 2^63
 * and clear C2. One of 2^63 or more they leave as it is, and the stack
 * too, and set C2. */
[[nodiscard]] bool reducible(CpuState &cpu, const X87Register &value) noexcept
{
	if ((value.sign_exponent & 0x7fffU) >= 0x3fffU + 63U &&
		!is_nan(value) && !is_infinity(value) && !is_unsupported(value)) {
		cpu.x87_status = static_cast<std::uint16_t>(
			(cpu.x87_status | status_c2) & ~status_c1);
		return false;
	}
	cpu.x87_status &= static_cast<std::uint16_t>(~status_c2);
	return true;
}

[[nodiscard]] X87Register transcendental(CpuState &cpu,
	TranscendentalOperation operation, X87Register value) noexcept
{
	auto state = soft_state(cpu, true);
	denormal_operand(state, value);
	extFloat80_t result{};
	switch (operation) {
	case TranscendentalOperation::sine:
		result = FEXCore::cephes_128bit::x87_fsin(&state, to_soft(value));
		break;
	case TranscendentalOperation::cosine:
		result = FEXCore::cephes_128bit::x87_fcos(&state, to_soft(value));
		break;
	case TranscendentalOperation::tangent:
		result = FEXCore::cephes_128bit::x87_ftan(&state, to_soft(value));
		break;
	}
	commit_exceptions(cpu, state);
	return from_soft(result);
}

/* F2XM1: 2^x - 1, where an operand beyond one is returned as it is. */
[[nodiscard]] X87Register f2xm1(CpuState &cpu,
	X87Register value) noexcept
{
	auto state = soft_state(cpu, true);
	denormal_operand(state, value);
	const auto result = FEXCore::cephes_128bit::x87_f2xm1(
		&state, to_soft(value));
	commit_exceptions(cpu, state);
	return from_soft(result);
}

/* FYL2X and FYL2XP1: y * log2(x) and y * log2(x + 1). */
[[nodiscard]] X87Register fyl2x(CpuState &cpu, X87Register value,
	X87Register multiplier, bool plus_one) noexcept
{
	auto state = soft_state(cpu, true);
	/* A NaN takes precedence over a denormal operand. */
	if (!is_nan(value) && !is_nan(multiplier)) {
		denormal_operand(state, value);
		denormal_operand(state, multiplier);
	}
	const auto result = FEXCore::cephes_128bit::x87_fyl2x(&state,
		to_soft(value), to_soft(multiplier), plus_one);
	commit_exceptions(cpu, state);
	return from_soft(result);
}

[[nodiscard]] X87Register fpatan(CpuState &cpu, X87Register y,
	X87Register x) noexcept
{
	auto state = soft_state(cpu, true);
	denormal_operand(state, y);
	denormal_operand(state, x);
	const auto result = FEXCore::cephes_128bit::atan2l(
		to_float128(state, y), to_float128(state, x));
	return inexact_result(cpu, state, result, Inexact::unless_zero);
}

/* FSCALE adds ST(1), truncated, to ST(0)'s exponent and rounds once
 * (`extF80_scale`), whatever the exponent's size: a factor of 2^n itself
 * overflows long before the result does. A NaN propagates as in a
 * multiplication; 2^+inf is an infinity, which a zero makes invalid, and
 * 2^-inf a zero, which an infinity makes invalid. */
[[nodiscard]] X87Register scale(CpuState &cpu, X87Register value,
	X87Register exponent) noexcept
{
	auto state = soft_state(cpu, true);
	denormal_operand(state, value);
	denormal_operand(state, exponent);
	extFloat80_t result{};
	if (is_nan(value) || is_nan(exponent)) {
		result = extF80_mul(&state, to_soft(value), to_soft(exponent));
	} else if (is_infinity(exponent)) {
		const X87Register infinity{0x8000000000000000ULL, 0x7fffU};
		result = (exponent.sign_exponent & 0x8000U) == 0 ?
			extF80_mul(&state, to_soft(value), to_soft(infinity)) :
			extF80_div(&state, to_soft(value), to_soft(infinity));
	} else {
		/* Any n beyond 2^16 saturates the result. */
		std::int32_t n = (exponent.sign_exponent & 0x8000U) != 0 ?
			-0x10000 : 0x10000;
		if ((exponent.sign_exponent & 0x7fffU) < 0x3fffU + 16U)
			n = static_cast<std::int32_t>(extF80_to_i32(&state,
				to_soft(exponent), softfloat_round_minMag, false));
		result = extF80_scale(&state, to_soft(value), n);
	}
	commit_exceptions(cpu, state);
	return from_soft(result);
}

/* FPREM (`nearest` false) and FPREM1: x87's exact remainder, partial for
 * operands whose exponents differ by 64 or more, which sets C2; a complete
 * one sets C0, C3 and C1 to the quotient's low three bits. An unmasked
 * exception the gadget leaves for leaves them as they were. */
[[nodiscard]] X87Register remainder(CpuState &cpu, X87Register dividend,
	X87Register divisor, bool nearest) noexcept
{
	auto state = soft_state(cpu, true);
	denormal_operand(state, dividend);
	denormal_operand(state, divisor);
	uint_fast16_t conditions{};
	const auto result = extF80_x87Rem(&state, to_soft(dividend),
		to_soft(divisor), nearest, &conditions);
	commit_exceptions(cpu, state);
	if (cpu.exception_raised == 0)
		cpu.x87_status = static_cast<std::uint16_t>(
			(cpu.x87_status & ~status_condition_codes) | conditions);
	return from_soft(result);
}

/* FXTRACT: the exponent of ST(0)'s normalized value and its significand
 * with the exponent of 1.0. Zero's exponent is negative infinity, which
 * raises division by zero; an infinity's is positive infinity; a NaN
 * comes back as both. */
void extract(CpuState &cpu, X87Register value, X87Register &exponent_result,
	X87Register &significand_result) noexcept
{
	auto state = soft_state(cpu, true);
	const auto sign = static_cast<std::uint16_t>(value.sign_exponent & 0x8000U);
	const auto exponent = value.sign_exponent & 0x7fffU;
	if (is_nan(value)) {
		if (is_signaling(value))
			state.exceptionFlags |= softfloat_flag_invalid;
		value.significand |= 0x4000000000000000ULL;
		exponent_result = value;
		significand_result = value;
	} else if (is_infinity(value)) {
		exponent_result = {0x8000000000000000ULL, 0x7fffU};
		significand_result = value;
	} else if (is_zero(value)) {
		state.exceptionFlags |= softfloat_flag_infinite;
		exponent_result = {0x8000000000000000ULL, 0xffffU};
		significand_result = value;
	} else if (exponent == 0x7fffU || (exponent != 0 &&
		(value.significand >> 63U) == 0)) {
		/* A pseudo-infinity, pseudo-NaN or unnormal is unsupported. */
		state.exceptionFlags |= softfloat_flag_invalid;
		exponent_result = quiet_nan();
		exponent_result.sign_exponent |= 0x8000U;
		significand_result = exponent_result;
	} else {
		auto significand = value.significand;
		std::int32_t unbiased = static_cast<std::int32_t>(exponent) - 16383;
		if (exponent == 0) {
			denormal_operand(state, value);
			/* A denormal's value is its significand times 2^-16445,
			 * a pseudo-denormal's the same as with exponent 1. */
			const auto shift = static_cast<std::int32_t>(
				std::countl_zero(significand));
			significand <<= shift;
			unbiased = -16382 - shift;
		}
		exponent_result = from_i32(unbiased);
		significand_result = {significand,
			static_cast<std::uint16_t>(sign | 0x3fffU)};
	}
	commit_exceptions(cpu, state);
}

enum class BinaryOperation : std::uint8_t {
	add,
	multiply,
	subtract,
	divide,
};

[[nodiscard]] X87Register binary(CpuState &cpu, BinaryOperation operation,
	X87Register left, X87Register right) noexcept
{
	auto state = soft_state(cpu);
	denormal_operand(state, left);
	denormal_operand(state, right);
	extFloat80_t result{};
	switch (operation) {
	case BinaryOperation::add:
		result = extF80_add(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::multiply:
		result = extF80_mul(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::subtract:
		result = extF80_sub(&state, to_soft(left), to_soft(right));
		break;
	case BinaryOperation::divide:
		result = extF80_div(&state, to_soft(left), to_soft(right));
		break;
	}
	commit_exceptions(cpu, state);
	return from_soft(result);
}

enum class CompareResult : std::uint8_t {
	greater,
	less,
	equal,
	unordered,
};

/* FCOM, FICOM, FCOMI and FTST raise invalid operation for any NaN, and the
 * quiet FUCOM and FUCOMI only for a signaling one; a NaN makes the compare
 * unordered and takes precedence over a denormal operand. */
[[nodiscard]] CompareResult compare(CpuState &cpu, X87Register left,
	X87Register right, bool quiet) noexcept
{
	auto state = soft_state(cpu);
	if (is_nan(left) || is_nan(right)) {
		if (!quiet || is_signaling(left) || is_signaling(right))
			state.exceptionFlags |= softfloat_flag_invalid;
		commit_exceptions(cpu, state);
		return CompareResult::unordered;
	}
	denormal_operand(state, left);
	denormal_operand(state, right);
	const bool equal = extF80_eq(&state, to_soft(left), to_soft(right));
	const bool less = !equal && extF80_lt(&state, to_soft(left), to_soft(right));
	commit_exceptions(cpu, state);
	return equal ? CompareResult::equal :
		(less ? CompareResult::less : CompareResult::greater);
}

[[nodiscard]] X87Register packed_bcd_load(CpuState &cpu,
	std::span<const std::byte> bytes) noexcept
{
	std::uint64_t value{};
	std::uint64_t multiplier{1};
	for (std::size_t index = 0; index < 9; ++index) {
		const auto byte = std::to_integer<std::uint8_t>(bytes[index]);
		value += (byte & 0xfU) * multiplier;
		multiplier *= 10;
		value += (byte >> 4U) * multiplier;
		multiplier *= 10;
	}
	auto result = from_soft(ui64_to_extF80(value));
	if ((std::to_integer<std::uint8_t>(bytes[9]) & 0x80U) != 0)
		result.sign_exponent |= 0x8000U;
	(void)cpu;
	return result;
}

/* FBSTP: a value that rounds to 18 decimal digits or fewer, inexact if it
 * rounded; any other, a NaN, an infinity and an unsupported encoding store
 * the packed BCD indefinite and raise invalid. */
void packed_bcd_store(CpuState &cpu, X87Register value,
	std::span<std::byte, 10> bytes) noexcept
{
	constexpr std::uint64_t limit = 1000000000000000000ULL;
	auto state = soft_state(cpu);
	const bool negative = (value.sign_exponent & 0x8000U) != 0;
	std::uint64_t integer{};
	bool valid = !is_unsupported(value) && !is_nan(value) &&
		!is_infinity(value);
	if (valid) {
		value = from_soft(extF80_roundToInt(&state, to_soft(value),
			state.roundingMode, true));
		value.sign_exponent &= 0x7fffU;
		integer = extF80_to_ui64(&state, to_soft(value),
			state.roundingMode, false);
		valid = integer < limit &&
			(state.exceptionFlags & softfloat_flag_invalid) == 0;
	}
	if (!valid) {
		state.exceptionFlags = static_cast<std::uint8_t>(
			(state.exceptionFlags & ~(softfloat_flag_inexact |
			softfloat_flag_roundedUp)) | softfloat_flag_invalid);
	}
	commit_exceptions(cpu, state);
	if (!valid) {
		for (std::size_t index = 0; index < 7; ++index)
			bytes[index] = std::byte{};
		bytes[7] = std::byte{0xc0};
		bytes[8] = std::byte{0xff};
		bytes[9] = std::byte{0xff};
		return;
	}
	for (std::size_t index = 0; index < 9; ++index) {
		const auto low = integer % 10U;
		integer /= 10U;
		const auto high = integer % 10U;
		integer /= 10U;
		bytes[index] = static_cast<std::byte>(low | (high << 4U));
	}
	bytes[9] = negative ? std::byte{0x80} : std::byte{};
}

} // namespace

extern "C" void uml_x86_64_x87_from_f32(CpuState *cpu,
	std::uint32_t bits, X87Register *result) noexcept
{
	*result = from_float32(*cpu, bits);
}

extern "C" void uml_x86_64_x87_from_f64(CpuState *cpu,
	std::uint64_t bits, X87Register *result) noexcept
{
	*result = from_float64(*cpu, bits);
}

extern "C" void uml_x86_64_x87_from_i32(
	std::int32_t value, X87Register *result) noexcept
{
	*result = from_i32(value);
}

extern "C" void uml_x86_64_x87_from_i64(
	std::int64_t value, X87Register *result) noexcept
{
	*result = from_i64(value);
}

extern "C" void uml_x86_64_x87_from_bcd(CpuState *cpu,
	const void *source, X87Register *result) noexcept
{
	*result = packed_bcd_load(*cpu, {
		static_cast<const std::byte *>(source), 10});
}

extern "C" std::uint32_t uml_x86_64_x87_to_f32(CpuState *cpu,
	const X87Register *value) noexcept
{
	return to_float32(*cpu, *value);
}

extern "C" std::uint64_t uml_x86_64_x87_to_f64(CpuState *cpu,
	const X87Register *value) noexcept
{
	return to_float64(*cpu, *value);
}

namespace {

/* FIST, FISTP and FISTTP raise precision for a value they round. */
[[nodiscard]] softfloat_state integer_store_state(
	const CpuState &cpu, bool truncate) noexcept
{
	auto state = soft_state(cpu);
	if (truncate)
		state.roundingMode = softfloat_round_minMag;
	return state;
}

} // namespace

extern "C" std::uint32_t uml_x86_64_x87_to_i16(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	auto converted = extF80_to_i32(&state, to_soft(*value),
		state.roundingMode, true);
	if (converted < std::numeric_limits<std::int16_t>::min() ||
		converted > std::numeric_limits<std::int16_t>::max()) {
		state.exceptionFlags |= softfloat_flag_invalid;
		converted = std::numeric_limits<std::int16_t>::min();
	}
	commit_exceptions(*cpu, state);
	return static_cast<std::uint16_t>(static_cast<std::int16_t>(converted));
}

extern "C" std::uint32_t uml_x86_64_x87_to_i32(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	const auto converted = extF80_to_i32(&state, to_soft(*value),
		state.roundingMode, true);
	commit_exceptions(*cpu, state);
	return static_cast<std::uint32_t>(converted);
}

extern "C" std::uint64_t uml_x86_64_x87_to_i64(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept
{
	auto state = integer_store_state(*cpu, truncate != 0);
	const auto converted = extF80_to_i64(&state, to_soft(*value),
		state.roundingMode, true);
	commit_exceptions(*cpu, state);
	return static_cast<std::uint64_t>(converted);
}

extern "C" void uml_x86_64_x87_to_bcd(CpuState *cpu,
	const X87Register *value, void *destination) noexcept
{
	packed_bcd_store(*cpu, *value, std::span<std::byte, 10>{
		static_cast<std::byte *>(destination), 10});
}

#define UML_X86_64_DEFINE_X87_BINARY(name, operation) \
	extern "C" void uml_x86_64_x87_##name(CpuState *cpu, \
		const X87Register *left, const X87Register *right, \
		X87Register *result) noexcept \
	{ \
		*result = binary(*cpu, BinaryOperation::operation, *left, *right); \
	}
UML_X86_64_DEFINE_X87_BINARY(add, add)
UML_X86_64_DEFINE_X87_BINARY(multiply, multiply)
UML_X86_64_DEFINE_X87_BINARY(subtract, subtract)
UML_X86_64_DEFINE_X87_BINARY(divide, divide)
#undef UML_X86_64_DEFINE_X87_BINARY

/* Bit 0 of `flags` marks the quiet compares, bit 1 FCOMI and FUCOMI,
 * which leave C1 as it was. */
extern "C" std::uint32_t uml_x86_64_x87_compare(CpuState *cpu,
	const X87Register *left, const X87Register *right,
	std::uint32_t flags) noexcept
{
	const auto c1 = static_cast<std::uint16_t>(cpu->x87_status &
		status_c1);
	const auto result = static_cast<std::uint32_t>(compare(*cpu, *left,
		*right, (flags & 1U) != 0));
	if ((flags & 2U) != 0)
		cpu->x87_status = static_cast<std::uint16_t>(
			(cpu->x87_status & ~status_c1) | c1);
	return result;
}

extern "C" void uml_x86_64_x87_f2xm1(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = f2xm1(*cpu, *input);
}

extern "C" void uml_x86_64_x87_fyl2x(CpuState *cpu,
	const X87Register *value, const X87Register *multiplier,
	X87Register *result) noexcept
{
	*result = fyl2x(*cpu, *value, *multiplier, false);
}

extern "C" void uml_x86_64_x87_fyl2xp1(CpuState *cpu,
	const X87Register *value, const X87Register *multiplier,
	X87Register *result) noexcept
{
	*result = fyl2x(*cpu, *value, *multiplier, true);
}

/* FPTAN's second result, the one it pushes, is 1.0 for a tangent; for an
 * operand that has none, the same NaN or indefinite again. */
extern "C" void uml_x86_64_x87_tangent(CpuState *cpu,
	const X87Register *input, X87Register *result,
	X87Register *second) noexcept
{
	if (!reducible(*cpu, *input)) {
		*result = *input;
		return;
	}
	*result = transcendental(*cpu, TranscendentalOperation::tangent, *input);
	*second = is_nan(*result) ? *result :
		X87Register{0x8000000000000000ULL, 0x3fffU};
}

extern "C" void uml_x86_64_x87_atan(CpuState *cpu,
	const X87Register *y, const X87Register *x,
	X87Register *result) noexcept
{
	*result = fpatan(*cpu, *y, *x);
}

extern "C" void uml_x86_64_x87_extract(CpuState *cpu,
	const X87Register *input, X87Register *exponent_result,
	X87Register *significand_result) noexcept
{
	extract(*cpu, *input, *exponent_result, *significand_result);
}

extern "C" void uml_x86_64_x87_fprem(CpuState *cpu,
	const X87Register *dividend, const X87Register *divisor,
	X87Register *result) noexcept
{
	*result = remainder(*cpu, *dividend, *divisor, false);
}

extern "C" void uml_x86_64_x87_fprem1(CpuState *cpu,
	const X87Register *dividend, const X87Register *divisor,
	X87Register *result) noexcept
{
	*result = remainder(*cpu, *dividend, *divisor, true);
}

extern "C" void uml_x86_64_x87_sqrt(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	auto state = soft_state(*cpu);
	denormal_operand(state, *input);
	*result = from_soft(extF80_sqrt(&state, to_soft(*input)));
	commit_exceptions(*cpu, state);
}

extern "C" void uml_x86_64_x87_sincos(CpuState *cpu,
	const X87Register *input, X87Register *sine_result,
	X87Register *cosine_result) noexcept
{
	if (!reducible(*cpu, *input)) {
		*sine_result = *input;
		return;
	}
	*sine_result = transcendental(
		*cpu, TranscendentalOperation::sine, *input);
	*cosine_result = transcendental(
		*cpu, TranscendentalOperation::cosine, *input);
}

extern "C" void uml_x86_64_x87_round(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	auto state = soft_state(*cpu);
	denormal_operand(state, *input);
	/* FRNDINT raises precision for an operand that is not an integer. */
	*result = from_soft(extF80_roundToInt(&state, to_soft(*input),
		state.roundingMode, true));
	commit_exceptions(*cpu, state);
}

extern "C" void uml_x86_64_x87_scale(CpuState *cpu,
	const X87Register *value, const X87Register *exponent,
	X87Register *result) noexcept
{
	*result = scale(*cpu, *value, *exponent);
}

extern "C" void uml_x86_64_x87_sine(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = reducible(*cpu, *input) ?
		transcendental(*cpu, TranscendentalOperation::sine, *input) :
		*input;
}

extern "C" void uml_x86_64_x87_cosine(CpuState *cpu,
	const X87Register *input, X87Register *result) noexcept
{
	*result = reducible(*cpu, *input) ?
		transcendental(*cpu, TranscendentalOperation::cosine, *input) :
		*input;
}

extern "C" std::uint32_t uml_x86_64_x87_image_tags(
	const CpuState *cpu) noexcept
{
	std::uint32_t tags{};
	for (unsigned physical = 0; physical < 8; ++physical) {
		const auto shift = physical * 2U;
		auto tag = (std::uint32_t{cpu->x87_tag} >> shift) & 3U;
		if (tag != 3U) {
			const auto &value = cpu->x87[physical];
			const auto exponent = value.sign_exponent & 0x7fffU;
			if (exponent == 0)
				tag = value.significand == 0 ? 1U : 2U;
			else if (exponent == 0x7fffU ||
				(value.significand >> 63U) == 0)
				tag = 2U;
			else
				tag = 0U;
		}
		tags |= tag << shift;
	}
	return tags;
}

} // namespace uml::cpu::x86_64::gadgets
