// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_encodings.hpp"

#include <cstdint>
#include <utility>

/*
 * The host's floating-point conversions for the library's VFP and Advanced
 * SIMD forms (semantics_fp.hpp): between single, double and half precision,
 * to and from 32-bit integers with a chosen rounding, and to and from
 * 32-bit fixed point. The fixed-point instructions encode the number of
 * fraction bits as an immediate, so each one is instantiated for 1 to 32
 * bits and selected at run time.
 */

namespace uml::cpu::arm64::aarch32::detail::fp {

[[nodiscard]] inline std::uint64_t single_to_double(std::uint32_t a) noexcept
{
	std::uint64_t r;
	__asm__ volatile("fmov s1, %w1\n\tfcvt d0, s1\n\tfmov %0, d0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}
[[nodiscard]] inline std::uint32_t double_to_single(std::uint64_t a) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov d1, %1\n\tfcvt s0, d1\n\tfmov %w0, s0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}
[[nodiscard]] inline std::uint32_t half_to_single(std::uint32_t a) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov s1, %w1\n\tfcvt s0, h1\n\tfmov %w0, s0" : "=r"(r) : "r"(a & 0xffffU) : "v0", "v1");
	return r;
}
[[nodiscard]] inline std::uint64_t half_to_double(std::uint32_t a) noexcept
{
	std::uint64_t r;
	__asm__ volatile("fmov s1, %w1\n\tfcvt d0, h1\n\tfmov %0, d0" : "=r"(r) : "r"(a & 0xffffU) : "v0", "v1");
	return r;
}
[[nodiscard]] inline std::uint32_t single_to_half(std::uint32_t a) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov s1, %w1\n\tfcvt h0, s1\n\tfmov %w0, s0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}
[[nodiscard]] inline std::uint32_t double_to_half(std::uint64_t a) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov d1, %1\n\tfcvt h0, d1\n\tfmov %w0, s0" : "=r"(r) : "r"(a) : "v0", "v1");
	return r;
}

/* Floating point to a 32-bit integer, rounding as the instruction names:
 * toward zero (z), to nearest even (n), toward plus infinity (p), toward
 * minus infinity (m); signed (s) or unsigned (u). */
#define UML_A32_FP_TO_INT(name, insn)                                                        \
	[[nodiscard]] inline std::uint32_t name(std::uint16_t a) noexcept                    \
	{                                                                                    \
		std::uint32_t r;                                                             \
		__asm__ volatile("fmov s1, %w1\n\t" insn " %w0, h1" : "=r"(r) : "r"(std::uint32_t{a}) : "v1"); \
		return r;                                                                    \
	}                                                                                    \
	[[nodiscard]] inline std::uint32_t name(std::uint32_t a) noexcept                    \
	{                                                                                    \
		std::uint32_t r;                                                             \
		__asm__ volatile("fmov s1, %w1\n\t" insn " %w0, s1" : "=r"(r) : "r"(a) : "v1"); \
		return r;                                                                    \
	}                                                                                    \
	[[nodiscard]] inline std::uint32_t name(std::uint64_t a) noexcept                    \
	{                                                                                    \
		std::uint32_t r;                                                             \
		__asm__ volatile("fmov d1, %1\n\t" insn " %w0, d1" : "=r"(r) : "r"(a) : "v1"); \
		return r;                                                                    \
	}

UML_A32_FP_TO_INT(to_int_zs, "fcvtzs")
UML_A32_FP_TO_INT(to_int_zu, "fcvtzu")
UML_A32_FP_TO_INT(to_int_ns, "fcvtns")
UML_A32_FP_TO_INT(to_int_nu, "fcvtnu")
UML_A32_FP_TO_INT(to_int_ps, "fcvtps")
UML_A32_FP_TO_INT(to_int_pu, "fcvtpu")
UML_A32_FP_TO_INT(to_int_ms, "fcvtms")
UML_A32_FP_TO_INT(to_int_mu, "fcvtmu")
UML_A32_FP_TO_INT(to_int_as, "fcvtas")
UML_A32_FP_TO_INT(to_int_au, "fcvtau")

#undef UML_A32_FP_TO_INT

/* Half precision to a 16-bit integer in an H register, as the A64 scalar
 * forms do: a result out of range saturates and is an invalid operation,
 * not an inexact one. Converting through 32 bits would need the Inexact
 * flag undone afterwards, and the lanes of one Advanced SIMD instruction
 * share the flags. */
#define UML_A32_HALF_TO_INT16(name, insn)                                                      \
	[[nodiscard]] inline std::uint16_t name(std::uint16_t a) noexcept                      \
	{                                                                                      \
		std::uint32_t r;                                                               \
		__asm__ volatile("fmov s1, %w1\n\t" insn " h0, h1\n\tfmov %w0, s0"                \
				 : "=r"(r) : "r"(std::uint32_t{a}) : "v0", "v1");              \
		return static_cast<std::uint16_t>(r);                                          \
	}

UML_A32_HALF_TO_INT16(to_int16_zs, "fcvtzs")
UML_A32_HALF_TO_INT16(to_int16_zu, "fcvtzu")
UML_A32_HALF_TO_INT16(to_int16_ns, "fcvtns")
UML_A32_HALF_TO_INT16(to_int16_nu, "fcvtnu")
UML_A32_HALF_TO_INT16(to_int16_ps, "fcvtps")
UML_A32_HALF_TO_INT16(to_int16_pu, "fcvtpu")
UML_A32_HALF_TO_INT16(to_int16_ms, "fcvtms")
UML_A32_HALF_TO_INT16(to_int16_mu, "fcvtmu")
UML_A32_HALF_TO_INT16(to_int16_as, "fcvtas")
UML_A32_HALF_TO_INT16(to_int16_au, "fcvtau")

#undef UML_A32_HALF_TO_INT16

/* Floating point to a 32-bit integer under the FPCR rounding mode `mode`
 * (the FPSCR RMode encoding: 0 nearest, 1 plus infinity, 2 minus infinity,
 * 3 zero). */
template<typename Float>
[[nodiscard]] inline std::uint32_t to_int(Float value, bool is_unsigned, unsigned mode) noexcept
{
	switch (mode & 3U) {
	case 0:
		return is_unsigned ? to_int_nu(value) : to_int_ns(value);
	case 1:
		return is_unsigned ? to_int_pu(value) : to_int_ps(value);
	case 2:
		return is_unsigned ? to_int_mu(value) : to_int_ms(value);
	default:
		return is_unsigned ? to_int_zu(value) : to_int_zs(value);
	}
}

/* A 32-bit integer to floating point, under the FPCR rounding mode. */
#define UML_A32_INT_TO_FP(name, insn, reg, result_type, move)                            \
	[[nodiscard]] inline result_type name(std::uint32_t a) noexcept                  \
	{                                                                                \
		result_type r;                                                           \
		__asm__ volatile(insn " " reg "0, %w1\n\t" move : "=r"(r) : "r"(a) : "v0"); \
		return r;                                                                \
	}

UML_A32_INT_TO_FP(s32_to_single, "scvtf", "s", std::uint32_t, "fmov %w0, s0")
UML_A32_INT_TO_FP(u32_to_single, "ucvtf", "s", std::uint32_t, "fmov %w0, s0")
UML_A32_INT_TO_FP(s32_to_double, "scvtf", "d", std::uint64_t, "fmov %0, d0")
UML_A32_INT_TO_FP(u32_to_double, "ucvtf", "d", std::uint64_t, "fmov %0, d0")
UML_A32_INT_TO_FP(s32_to_half, "scvtf", "h", std::uint32_t, "fmov %w0, s0")
UML_A32_INT_TO_FP(u32_to_half, "ucvtf", "h", std::uint32_t, "fmov %w0, s0")

#undef UML_A32_INT_TO_FP

/* The fixed-point forms, one instantiation per number of fraction bits. */
#define UML_A32_FIXED_OP(name, in_type, out_type, text)                                 \
	template<int Fraction>                                                          \
	struct name final {                                                             \
		[[nodiscard]] static out_type apply(in_type a) noexcept                 \
		{                                                                       \
			out_type r;                                                     \
			__asm__ volatile(text : "=r"(r) : "r"(a), "i"(Fraction) : "v0", "v1"); \
			return r;                                                       \
		}                                                                       \
	};

UML_A32_FIXED_OP(SingleToFixedS, std::uint32_t, std::uint32_t, "fmov s1, %w1\n\tfcvtzs %w0, s1, #%2")
UML_A32_FIXED_OP(SingleToFixedU, std::uint32_t, std::uint32_t, "fmov s1, %w1\n\tfcvtzu %w0, s1, #%2")
UML_A32_FIXED_OP(DoubleToFixedS, std::uint64_t, std::uint32_t, "fmov d1, %1\n\tfcvtzs %w0, d1, #%2")
UML_A32_FIXED_OP(DoubleToFixedU, std::uint64_t, std::uint32_t, "fmov d1, %1\n\tfcvtzu %w0, d1, #%2")
UML_A32_FIXED_OP(FixedSToSingle, std::uint32_t, std::uint32_t, "scvtf s0, %w1, #%2\n\tfmov %w0, s0")
UML_A32_FIXED_OP(FixedUToSingle, std::uint32_t, std::uint32_t, "ucvtf s0, %w1, #%2\n\tfmov %w0, s0")
UML_A32_FIXED_OP(FixedSToDouble, std::uint32_t, std::uint64_t, "scvtf d0, %w1, #%2\n\tfmov %0, d0")
UML_A32_FIXED_OP(FixedUToDouble, std::uint32_t, std::uint64_t, "ucvtf d0, %w1, #%2\n\tfmov %0, d0")
UML_A32_FIXED_OP(HalfToFixedS, std::uint32_t, std::uint32_t, "fmov s1, %w1\n\tfcvtzs %w0, h1, #%2")
UML_A32_FIXED_OP(HalfToFixedU, std::uint32_t, std::uint32_t, "fmov s1, %w1\n\tfcvtzu %w0, h1, #%2")
UML_A32_FIXED_OP(HalfToFixed16S, std::uint32_t, std::uint32_t,
	"fmov s1, %w1\n\tfcvtzs h0, h1, #%2\n\tfmov %w0, s0")
UML_A32_FIXED_OP(HalfToFixed16U, std::uint32_t, std::uint32_t,
	"fmov s1, %w1\n\tfcvtzu h0, h1, #%2\n\tfmov %w0, s0")
UML_A32_FIXED_OP(FixedSToHalf, std::uint32_t, std::uint32_t, "scvtf h0, %w1, #%2\n\tfmov %w0, s0")
UML_A32_FIXED_OP(FixedUToHalf, std::uint32_t, std::uint32_t, "ucvtf h0, %w1, #%2\n\tfmov %w0, s0")

#undef UML_A32_FIXED_OP

template<template<int> typename Op, typename In, typename Out, std::size_t... Index>
[[nodiscard]] inline Out fixed_dispatch(unsigned fraction, In a, std::index_sequence<Index...>) noexcept
{
	Out result{};
	static_cast<void>(
		((fraction == Index + 1U ? (result = Op<static_cast<int>(Index) + 1>::apply(a), true) : false) || ...));
	return result;
}

/* Fraction bits 1 to `Limit`: 32, or 16 for the forms on an H register. */
template<template<int> typename Op, typename In, typename Out, std::size_t Limit = 32>
[[nodiscard]] inline Out fixed_convert(unsigned fraction, In a) noexcept
{
	return fixed_dispatch<Op, In, Out>(fraction, a, std::make_index_sequence<Limit>{});
}

/* Floating point to a 32-bit integer under a rounding the instruction names
 * (FPDecodeRM): nearest even, plus infinity, minus infinity, zero, or
 * nearest with ties away. */
template<typename Float>
[[nodiscard]] inline std::uint32_t to_int_rounded(Float value, bool is_unsigned,
	FPRounding rounding) noexcept
{
	switch (rounding) {
	case FPRounding::POSINF:
		return is_unsigned ? to_int_pu(value) : to_int_ps(value);
	case FPRounding::NEGINF:
		return is_unsigned ? to_int_mu(value) : to_int_ms(value);
	case FPRounding::ZERO:
		return is_unsigned ? to_int_zu(value) : to_int_zs(value);
	case FPRounding::TIEAWAY:
		return is_unsigned ? to_int_au(value) : to_int_as(value);
	default:
		return is_unsigned ? to_int_nu(value) : to_int_ns(value);
	}
}

/* The same for a half-precision value to a 16-bit integer. */
[[nodiscard]] inline std::uint16_t to_int16_rounded(std::uint16_t value, bool is_unsigned,
	FPRounding rounding) noexcept
{
	switch (rounding) {
	case FPRounding::POSINF:
		return is_unsigned ? to_int16_pu(value) : to_int16_ps(value);
	case FPRounding::NEGINF:
		return is_unsigned ? to_int16_mu(value) : to_int16_ms(value);
	case FPRounding::ZERO:
		return is_unsigned ? to_int16_zu(value) : to_int16_zs(value);
	case FPRounding::TIEAWAY:
		return is_unsigned ? to_int16_au(value) : to_int16_as(value);
	default:
		return is_unsigned ? to_int16_nu(value) : to_int16_ns(value);
	}
}

} // namespace uml::cpu::arm64::aarch32::detail::fp
