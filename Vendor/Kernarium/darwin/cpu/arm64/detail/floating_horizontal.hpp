// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "floating_shape.hpp"

namespace uml::cpu::arm64::detail {

enum class FloatingHorizontalOperation : unsigned {
	absolute_difference,
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
};

enum class FloatingHorizontalForm : unsigned {
	scalar_binary,
	vector_binary,
	scalar_pairwise,
	vector_pairwise,
	reduction,
};

struct FloatingHorizontal final {
	bool recognized{};
	bool valid{};
	FloatingHorizontalOperation operation{};
	FloatingHorizontalForm form{};
	unsigned width{};
	unsigned vector_bytes{};
};

[[nodiscard]] constexpr FloatingHorizontal decode_floating_horizontal(
	std::uint32_t instruction) noexcept
{
	/* Scalar FABD, including its separate FP16 allocation. */
	if ((instruction & 0xffe0'fc00U) == 0x7ec0'1400U)
		return {true, true, FloatingHorizontalOperation::absolute_difference,
			FloatingHorizontalForm::scalar_binary, 16U, 16U};
	if ((instruction & 0xff20'fc00U) == 0x7e20'd400U)
		return {true, (instruction & 0x0080'0000U) != 0U,
			FloatingHorizontalOperation::absolute_difference,
			FloatingHorizontalForm::scalar_binary,
			(instruction & 0x0040'0000U) != 0U ? 64U : 32U, 16U};

	/* Scalar FMAX/FMAXNM/FMIN/FMINNM. ftype == 10 is reserved. */
	FloatingHorizontalOperation operation;
	bool matched = true;
	switch (instruction & 0xff20'fc00U) {
	case 0x1e20'4800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x1e20'6800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x1e20'5800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x1e20'7800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type != 2U, operation,
			FloatingHorizontalForm::scalar_binary,
			floating_width_from_type(type), 16U};
	}

	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* FEAT_FAMINMAX FAMAX and FAMIN: the virtual CPU does not implement
	 * FEAT_FAMINMAX, so both engines reject them as undefined. */
	if ((instruction & 0x9fe0'fc00U) == 0x0ec0'1c00U ||
	    (instruction & 0x9fa0'fc00U) == 0x0ea0'dc00U)
		return {true, false};
	/* Vector FP16 FABD/FADDP and min/max families. */
	matched = true;
	switch (instruction & 0xbfe0'fc00U) {
	case 0x2ec0'1400U:
		operation = FloatingHorizontalOperation::absolute_difference; break;
	case 0x2e40'1400U: operation = FloatingHorizontalOperation::add; break;
	case 0x0e40'3400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e40'3400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e40'0400U:
	case 0x2e40'0400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0ec0'3400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2ec0'3400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0ec0'0400U:
	case 0x2ec0'0400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool pairwise = operation == FloatingHorizontalOperation::add ||
			(operation != FloatingHorizontalOperation::absolute_difference &&
			 (instruction & 0x2000'0000U) != 0U);
		return {true, true, operation, pairwise ?
			FloatingHorizontalForm::vector_pairwise :
			FloatingHorizontalForm::vector_binary,
			16U, q ? 16U : 8U};
	}

	/* Vector FP32/FP64 forms. The .1D allocation is reserved. */
	matched = true;
	switch (instruction & 0xbfa0'fc00U) {
	case 0x2ea0'd400U:
		operation = FloatingHorizontalOperation::absolute_difference; break;
	case 0x2e20'd400U: operation = FloatingHorizontalOperation::add; break;
	case 0x0e20'f400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e20'c400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0ea0'f400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0ea0'c400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	case 0x2e20'f400U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e20'c400U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x2ea0'f400U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2ea0'c400U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool pairwise = (instruction & 0x2000'0000U) != 0U &&
			operation != FloatingHorizontalOperation::absolute_difference;
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, operation, pairwise ?
			FloatingHorizontalForm::vector_pairwise :
			FloatingHorizontalForm::vector_binary,
			is_double ? 64U : 32U, q ? 16U : 8U};
	}

	/* Scalar pairwise forms. The FP16 sz == 1 allocation is reserved. */
	matched = true;
	switch (instruction & 0xffbf'fc00U) {
	case 0x5e30'd800U:
	case 0x7e30'd800U: operation = FloatingHorizontalOperation::add; break;
	case 0x5e30'f800U:
	case 0x7e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x5e30'c800U:
	case 0x7e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x5eb0'f800U:
	case 0x7eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x5eb0'c800U:
	case 0x7eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool half = (instruction & 0x2000'0000U) == 0U;
		const bool size = (instruction & 0x0040'0000U) != 0U;
		return {true, !half || !size, operation,
			FloatingHorizontalForm::scalar_pairwise,
			half ? 16U : size ? 64U : 32U, 16U};
	}

	/* Across-vector reductions: FP16 accepts 4H/8H; FP32 requires 4S. */
	matched = true;
	switch (instruction & 0xbfff'fc00U) {
	case 0x0e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x0e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x0eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x0eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, operation, FloatingHorizontalForm::reduction,
			16U, q ? 16U : 8U};

	matched = true;
	switch (instruction & 0xbfbf'fc00U) {
	case 0x2e30'f800U: operation = FloatingHorizontalOperation::maximum; break;
	case 0x2e30'c800U:
		operation = FloatingHorizontalOperation::maximum_number; break;
	case 0x2eb0'f800U: operation = FloatingHorizontalOperation::minimum; break;
	case 0x2eb0'c800U:
		operation = FloatingHorizontalOperation::minimum_number; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool valid = q && (instruction & 0x0040'0000U) == 0U;
		return {true, valid, operation, FloatingHorizontalForm::reduction,
			32U, 16U};
	}
	return {};
}

} // namespace uml::cpu::arm64::detail
