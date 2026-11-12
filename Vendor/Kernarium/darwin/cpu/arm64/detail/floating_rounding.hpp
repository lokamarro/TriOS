// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

/*
 * How an A64 FP&SIMD data-processing form rounds its floating-point result,
 * as far as the trapped Underflow exception needs to know (FPRoundBase in the
 * 2026-06 pseudocode). A rounded form passes each result element through
 * FPRoundBase, so with FPCR.UFE set an exact tiny result raises Underflow,
 * and a result that its format's flush-to-zero control flushes raises only
 * FPSR.UFC, which never traps. An estimate (FRECPE) builds its result
 * without rounding; its only Underflow is that flush. Every other form,
 * integer, permute, move, absolute value, negation, comparison, conversion
 * to an integer and round to integral among them, produces no tiny result.
 */
enum class FloatingResultKind : std::uint8_t {
	none,
	rounded,
	estimate,
};

enum class FloatingFormat : std::uint8_t {
	none,
	half,
	single,
	double_precision,
	bfloat16,
};

enum class FloatingReduction : std::uint8_t {
	none,
	maximum,
	minimum,
	maximum_number,
	minimum_number,
};

struct FloatingRounding final {
	FloatingResultKind kind{FloatingResultKind::none};
	FloatingFormat format{FloatingFormat::none};
	/* The bytes of Vd that hold the form's results: the first element of a
	 * scalar form, which FPCR.NEP may merge the rest of Vd around, the low
	 * 8 or all 16 bytes of a vector form, and the high 8 bytes of a
	 * narrowing form's second-part variant. */
	std::uint8_t first_byte{};
	std::uint8_t bytes{};
	/* A conversion to half precision rounds through FPRoundCV, which
	 * ignores FPCR.FZ16. */
	bool ignores_fz16{};
	/* Under FPCR.AH, FMAX and FMIN never flush their result, and the
	 * reciprocal step, estimate and BFloat16 forms raise no Underflow of
	 * their own: they flush it or raise no exception at all. */
	bool unflushed_under_ah{};
	bool flushed_under_ah{};
	/* FMAXV, FMINV, FMAXNMV and FMINNMV round every step of their
	 * reduction, so a tiny intermediate maximum or minimum raises
	 * Underflow too; their source is Vn's low `reduced_bytes`. */
	FloatingReduction reduction{FloatingReduction::none};
	std::uint8_t reduced_bytes{};

	[[nodiscard]] constexpr bool operator==(
		const FloatingRounding &) const noexcept = default;
};

namespace floating_rounding_detail {

inline constexpr std::uint8_t ignores_fz16 = 1U;
inline constexpr std::uint8_t unflushed_under_ah = 2U;
inline constexpr std::uint8_t flushed_under_ah = 4U;

[[nodiscard]] constexpr unsigned field(
	std::uint32_t instruction, unsigned low, unsigned width) noexcept
{
	return (instruction >> low) & ((1U << width) - 1U);
}

[[nodiscard]] constexpr unsigned format_bytes(FloatingFormat format) noexcept
{
	switch (format) {
	case FloatingFormat::half:
	case FloatingFormat::bfloat16:
		return 2U;
	case FloatingFormat::single:
		return 4U;
	case FloatingFormat::double_precision:
		return 8U;
	case FloatingFormat::none:
		break;
	}
	return 0U;
}

[[nodiscard]] constexpr FloatingRounding make(FloatingResultKind kind,
	FloatingFormat format, unsigned first_byte, unsigned bytes,
	std::uint8_t flags) noexcept
{
	if (format == FloatingFormat::none)
		return {};
	return {
		.kind = kind,
		.format = format,
		.first_byte = static_cast<std::uint8_t>(first_byte),
		.bytes = static_cast<std::uint8_t>(bytes),
		.ignores_fz16 = (flags & ignores_fz16) != 0U,
		.unflushed_under_ah = (flags & unflushed_under_ah) != 0U,
		.flushed_under_ah = (flags & flushed_under_ah) != 0U,
	};
}

[[nodiscard]] constexpr FloatingRounding scalar(FloatingFormat format,
	std::uint8_t flags = 0U,
	FloatingResultKind kind = FloatingResultKind::rounded) noexcept
{
	return make(kind, format, 0U, format_bytes(format), flags);
}

[[nodiscard]] constexpr FloatingRounding vector(std::uint32_t instruction,
	FloatingFormat format, std::uint8_t flags = 0U,
	FloatingResultKind kind = FloatingResultKind::rounded) noexcept
{
	return make(kind, format, 0U, field(instruction, 30, 1) != 0U ? 16U : 8U,
		flags);
}

/* FCVTN, FCVTXN and BFCVTN write the low half of Vd and clear the rest;
 * their second-part variants (Q = 1) write the high half and keep the low
 * one. */
[[nodiscard]] constexpr FloatingRounding narrowing(
	std::uint32_t instruction, FloatingFormat format,
	std::uint8_t flags = 0U) noexcept
{
	return make(FloatingResultKind::rounded, format,
		field(instruction, 30, 1) != 0U ? 8U : 0U, 8U, flags);
}

/* ftype, or sz with size<1> clear: single or double precision. */
[[nodiscard]] constexpr FloatingFormat single_or_double(bool sz) noexcept
{
	return sz ? FloatingFormat::double_precision : FloatingFormat::single;
}

[[nodiscard]] constexpr FloatingFormat ftype_format(unsigned ftype) noexcept
{
	switch (ftype) {
	case 0U:
		return FloatingFormat::single;
	case 1U:
		return FloatingFormat::double_precision;
	case 3U:
		return FloatingFormat::half;
	default:
		return FloatingFormat::none;
	}
}

/* The element size a fixed-point shift's immh selects. */
[[nodiscard]] constexpr FloatingFormat immh_format(unsigned immh) noexcept
{
	if ((immh & 8U) != 0U)
		return FloatingFormat::double_precision;
	if ((immh & 4U) != 0U)
		return FloatingFormat::single;
	if ((immh & 2U) != 0U)
		return FloatingFormat::half;
	return FloatingFormat::none;
}

/* The size field of a by-element form: 00 half, 10 single, 11 double. */
[[nodiscard]] constexpr FloatingFormat element_format(unsigned size) noexcept
{
	switch (size) {
	case 0U:
		return FloatingFormat::half;
	case 2U:
		return FloatingFormat::single;
	case 3U:
		return FloatingFormat::double_precision;
	default:
		return FloatingFormat::none;
	}
}

/* Conversion between floating-point and fixed-point or integer, and
 * floating-point data-processing with one, two or three sources. */
[[nodiscard]] constexpr FloatingRounding scalar_floating(
	std::uint32_t instruction) noexcept
{
	const FloatingFormat format = ftype_format(field(instruction, 22, 2));
	if (field(instruction, 29, 1) != 0U)
		return {};
	if (field(instruction, 24, 1) != 0U) {
		/* FMADD, FMSUB, FNMADD, FNMSUB */
		if (field(instruction, 31, 1) != 0U)
			return {};
		return scalar(format);
	}
	const unsigned rmode = field(instruction, 19, 2);
	const unsigned conversion = field(instruction, 16, 3);
	if (field(instruction, 21, 1) == 0U) {
		/* SCVTF and UCVTF (scalar, fixed-point) */
		if (rmode == 0U && (conversion == 2U || conversion == 3U))
			return scalar(format);
		return {};
	}
	if (field(instruction, 10, 6) == 0U) {
		/* SCVTF and UCVTF (scalar, integer) */
		if (rmode == 0U && (conversion == 2U || conversion == 3U))
			return scalar(format);
		return {};
	}
	if (field(instruction, 31, 1) != 0U)
		return {};
	if (field(instruction, 10, 5) == 0b10000U) {
		switch (field(instruction, 15, 6)) {
		case 0b000011U: /* FSQRT */
			return scalar(format);
		case 0b000100U: /* FCVT to single precision */
			return format == FloatingFormat::single ?
				FloatingRounding{} :
				scalar(FloatingFormat::single);
		case 0b000101U: /* FCVT to double precision */
			return format == FloatingFormat::double_precision ?
				FloatingRounding{} :
				scalar(FloatingFormat::double_precision);
		case 0b000110U: /* BFCVT */
			return format == FloatingFormat::double_precision ?
				scalar(FloatingFormat::bfloat16,
					flushed_under_ah) :
				FloatingRounding{};
		case 0b000111U: /* FCVT to half precision */
			return format == FloatingFormat::half ?
				FloatingRounding{} :
				scalar(FloatingFormat::half, ignores_fz16);
		default:
			return {};
		}
	}
	if (field(instruction, 10, 2) == 0b10U) {
		const unsigned opcode = field(instruction, 12, 4);
		/* FMUL, FDIV, FADD, FSUB, FMAXNM, FMINNM, FNMUL */
		if (opcode <= 3U || opcode == 6U || opcode == 7U ||
			opcode == 8U)
			return scalar(format);
		/* FMAX, FMIN */
		if (opcode <= 5U)
			return scalar(format, unflushed_under_ah);
	}
	return {};
}

/* Advanced SIMD scalar: three same, three same FP16, two-register
 * miscellaneous, its FP16 form, pairwise, shift by immediate and x indexed
 * element. */
[[nodiscard]] constexpr FloatingRounding advsimd_scalar(
	std::uint32_t instruction) noexcept
{
	const bool u = field(instruction, 29, 1) != 0U;
	const bool a = field(instruction, 23, 1) != 0U;
	const bool sz = field(instruction, 22, 1) != 0U;
	const FloatingFormat sized = single_or_double(sz);
	if ((instruction & 0xdf20'0400U) == 0x5e20'0400U) {
		const unsigned opcode = field(instruction, 11, 5);
		if (!u && !a && opcode == 0b11011U) /* FMULX */
			return scalar(sized);
		if (!u && opcode == 0b11111U) /* FRECPS, FRSQRTS */
			return scalar(sized, flushed_under_ah);
		if (u && a && opcode == 0b11010U) /* FABD */
			return scalar(sized);
		return {};
	}
	if ((instruction & 0xdf60'c400U) == 0x5e40'0400U) {
		const unsigned opcode = field(instruction, 11, 3);
		if (!u && !a && opcode == 3U) /* FMULX */
			return scalar(FloatingFormat::half);
		if (!u && opcode == 7U) /* FRECPS, FRSQRTS */
			return scalar(FloatingFormat::half, flushed_under_ah);
		if (u && a && opcode == 2U) /* FABD */
			return scalar(FloatingFormat::half);
		return {};
	}
	if ((instruction & 0xdf3e'0c00U) == 0x5e20'0800U) {
		const unsigned opcode = field(instruction, 12, 5);
		if (opcode == 0b11101U) {
			if (!a) /* SCVTF, UCVTF */
				return scalar(sized);
			if (!u) /* FRECPE */
				return scalar(sized, flushed_under_ah,
					FloatingResultKind::estimate);
			return {};
		}
		if (u && opcode == 0b10110U && field(instruction, 22, 2) == 1U)
			return scalar(FloatingFormat::single); /* FCVTXN */
		return {};
	}
	if ((instruction & 0xdf7e'0c00U) == 0x5e78'0800U) {
		if (field(instruction, 12, 5) != 0b11101U)
			return {};
		if (!a) /* SCVTF, UCVTF */
			return scalar(FloatingFormat::half);
		if (!u) /* FRECPE */
			return scalar(FloatingFormat::half, flushed_under_ah,
				FloatingResultKind::estimate);
		return {};
	}
	if ((instruction & 0xdf3e'0c00U) == 0x5e30'0800U) {
		const unsigned opcode = field(instruction, 12, 5);
		const FloatingFormat format =
			u ? sized : (sz ? FloatingFormat::none :
					  FloatingFormat::half);
		/* FADDP, FMAXNMP, FMINNMP */
		if ((opcode == 0b01101U && !a) || opcode == 0b01100U)
			return scalar(format);
		if (opcode == 0b01111U) /* FMAXP, FMINP */
			return scalar(format, unflushed_under_ah);
		return {};
	}
	if ((instruction & 0xdf80'0400U) == 0x5f00'0400U) {
		/* SCVTF, UCVTF (scalar, fixed-point) */
		if (field(instruction, 11, 5) == 0b11100U)
			return scalar(immh_format(field(instruction, 19, 4)));
		return {};
	}
	if ((instruction & 0xdf00'0400U) == 0x5f00'0000U) {
		const unsigned opcode = field(instruction, 12, 4);
		/* FMLA, FMLS, FMUL, FMULX (by element) */
		if ((!u && (opcode == 1U || opcode == 5U || opcode == 9U)) ||
			(u && opcode == 9U))
			return scalar(element_format(field(instruction, 22, 2)));
		return {};
	}
	return {};
}

/* Advanced SIMD vector forms. */
[[nodiscard]] constexpr FloatingRounding advsimd_vector(
	std::uint32_t instruction) noexcept
{
	const bool u = field(instruction, 29, 1) != 0U;
	const bool a = field(instruction, 23, 1) != 0U;
	const bool sz = field(instruction, 22, 1) != 0U;
	const unsigned size = field(instruction, 22, 2);
	const FloatingFormat sized = single_or_double(sz);
	if ((instruction & 0x9f20'0400U) == 0x0e20'0400U) {
		const unsigned opcode = field(instruction, 11, 5);
		switch (opcode) {
		case 0b11000U: /* FMAXNM, FMINNM, FMAXNMP, FMINNMP */
			return vector(instruction, sized);
		case 0b11110U: /* FMAX, FMIN, FMAXP, FMINP */
			return vector(instruction, sized, unflushed_under_ah);
		case 0b11001U:
			if (!u) /* FMLA, FMLS */
				return vector(instruction, sized);
			/* FMLAL2, FMLSL2 */
			return (size & 1U) == 0U ?
				vector(instruction, FloatingFormat::single) :
				FloatingRounding{};
		case 0b11010U: /* FADD, FSUB, FADDP, FABD */
			return vector(instruction, sized);
		case 0b11011U: /* FMULX, FMUL */
			return a ? FloatingRounding{} :
				   vector(instruction, sized);
		case 0b11101U: /* FMLAL, FMLSL */
			return !u && (size & 1U) == 0U ?
				vector(instruction, FloatingFormat::single) :
				FloatingRounding{};
		case 0b11111U:
			if (!u) /* FRECPS, FRSQRTS */
				return vector(instruction, sized,
					flushed_under_ah);
			/* FDIV */
			return a ? FloatingRounding{} :
				   vector(instruction, sized);
		default:
			return {};
		}
	}
	if ((instruction & 0x9f60'c400U) == 0x0e40'0400U) {
		const FloatingFormat half = FloatingFormat::half;
		switch (field(instruction, 11, 3)) {
		case 0U: /* FMAXNM, FMINNM, FMAXNMP, FMINNMP */
			return vector(instruction, half);
		case 6U: /* FMAX, FMIN, FMAXP, FMINP */
			return vector(instruction, half, unflushed_under_ah);
		case 1U: /* FMLA, FMLS */
			return u ? FloatingRounding{} : vector(instruction, half);
		case 2U: /* FADD, FSUB, FADDP, FABD */
			return vector(instruction, half);
		case 3U: /* FMULX, FMUL */
			return a ? FloatingRounding{} : vector(instruction, half);
		case 7U:
			if (!u) /* FRECPS, FRSQRTS */
				return vector(instruction, half,
					flushed_under_ah);
			/* FDIV */
			return a ? FloatingRounding{} : vector(instruction, half);
		default:
			return {};
		}
	}
	if ((instruction & 0x9f3e'0c00U) == 0x0e20'0800U) {
		const unsigned opcode = field(instruction, 12, 5);
		if (opcode == 0b10110U) {
			if (!u && size == 0U) /* FCVTN to half precision */
				return narrowing(instruction,
					FloatingFormat::half, ignores_fz16);
			if (!u && size == 1U) /* FCVTN to single precision */
				return narrowing(instruction,
					FloatingFormat::single);
			if (!u && size == 2U) /* BFCVTN */
				return narrowing(instruction,
					FloatingFormat::bfloat16,
					flushed_under_ah);
			if (u && size == 1U) /* FCVTXN */
				return narrowing(instruction,
					FloatingFormat::single);
			return {};
		}
		if (opcode == 0b11101U) {
			if (!a) /* SCVTF, UCVTF */
				return vector(instruction, sized);
			if (!u) /* FRECPE */
				return vector(instruction, sized,
					flushed_under_ah,
					FloatingResultKind::estimate);
			return {};
		}
		if (u && a && opcode == 0b11111U) /* FSQRT */
			return vector(instruction, sized);
		return {};
	}
	if ((instruction & 0x9f7e'0c00U) == 0x0e78'0800U) {
		const unsigned opcode = field(instruction, 12, 5);
		if (opcode == 0b11101U) {
			if (!a) /* SCVTF, UCVTF */
				return vector(instruction, FloatingFormat::half);
			if (!u) /* FRECPE */
				return vector(instruction, FloatingFormat::half,
					flushed_under_ah,
					FloatingResultKind::estimate);
			return {};
		}
		if (u && a && opcode == 0b11111U) /* FSQRT */
			return vector(instruction, FloatingFormat::half);
		return {};
	}
	if ((instruction & 0x9f3e'0c00U) == 0x0e30'0800U) {
		/* FMAXNMV, FMINNMV, FMAXV, FMINV */
		const unsigned opcode = field(instruction, 12, 5);
		if (opcode != 0b01100U && opcode != 0b01111U)
			return {};
		const FloatingFormat format = sz ? FloatingFormat::none :
			u ? FloatingFormat::single :
			    FloatingFormat::half;
		const bool number = opcode == 0b01100U;
		FloatingRounding rounding =
			scalar(format, number ? 0U : unflushed_under_ah);
		if (rounding.kind == FloatingResultKind::none)
			return rounding;
		rounding.reduction = number ?
			(a ? FloatingReduction::minimum_number :
			     FloatingReduction::maximum_number) :
			(a ? FloatingReduction::minimum :
			     FloatingReduction::maximum);
		rounding.reduced_bytes = field(instruction, 30, 1) != 0U ? 16U : 8U;
		return rounding;
	}
	if ((instruction & 0x9f80'0400U) == 0x0f00'0400U) {
		/* SCVTF, UCVTF (vector, fixed-point) */
		if (field(instruction, 11, 5) == 0b11100U)
			return vector(instruction,
				immh_format(field(instruction, 19, 4)));
		return {};
	}
	if ((instruction & 0x9f00'0400U) == 0x0f00'0000U) {
		const unsigned opcode = field(instruction, 12, 4);
		if (!u) {
			/* FMLA, FMLS, FMUL (by element) */
			if (opcode == 1U || opcode == 5U || opcode == 9U)
				return vector(instruction, element_format(size));
			/* FMLAL, FMLSL (by element) */
			if ((opcode == 0U || opcode == 4U) && size == 2U)
				return vector(instruction,
					FloatingFormat::single);
			/* BFMLALB, BFMLALT (by element) */
			if (opcode == 0b1111U && size == 3U)
				return make(FloatingResultKind::rounded,
					FloatingFormat::single, 0U, 16U,
					flushed_under_ah);
			return {};
		}
		/* FMULX (by element) */
		if (opcode == 9U)
			return vector(instruction, element_format(size));
		/* FMLAL2, FMLSL2 (by element) */
		if ((opcode == 8U || opcode == 12U) && size == 2U)
			return vector(instruction, FloatingFormat::single);
		/* FCMLA (by element) */
		if ((opcode & 0b1001U) == 0b0001U && (size == 1U || size == 2U))
			return vector(instruction,
				size == 1U ? FloatingFormat::half :
					     FloatingFormat::single);
		return {};
	}
	if ((instruction & 0x9f20'8400U) == 0x0e00'8400U) {
		if (!u)
			return {};
		const unsigned opcode = field(instruction, 11, 4);
		const FloatingFormat format = size == 1U ?
			FloatingFormat::half :
			size == 2U ? FloatingFormat::single :
			size == 3U ? FloatingFormat::double_precision :
				     FloatingFormat::none;
		/* FCMLA, FCADD */
		if ((opcode & 0b1100U) == 0b1000U ||
			(opcode & 0b1101U) == 0b1100U)
			return vector(instruction, format);
		/* BFMLALB, BFMLALT */
		if (opcode == 0b1111U && size == 3U)
			return make(FloatingResultKind::rounded,
				FloatingFormat::single, 0U, 16U,
				flushed_under_ah);
		return {};
	}
	return {};
}

} // namespace floating_rounding_detail

/* The rounding of `instruction`'s floating-point result, or none for a word
 * outside the register-only FP&SIMD data-processing forms that round. */
[[nodiscard]] constexpr FloatingRounding classify_floating_rounding(
	std::uint32_t instruction) noexcept
{
	namespace d = floating_rounding_detail;
	if ((instruction & 0x5e00'0000U) == 0x1e00'0000U)
		return d::scalar_floating(instruction);
	if ((instruction & 0xde00'0000U) == 0x5e00'0000U)
		return d::advsimd_scalar(instruction);
	if ((instruction & 0x9e00'0000U) == 0x0e00'0000U)
		return d::advsimd_vector(instruction);
	return {};
}

} // namespace uml::cpu::arm64::detail
