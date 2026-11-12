// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/cpu_state.hpp"

#include <cstdint>

namespace uml::cpu::x86_64::gadgets {

/* Exact software primitives for formats and operations that arm64 has no
 * architectural equivalent for. Handwritten assembly gadgets retain all x87
 * decode, stack, tag, flag, memory, fault, and retirement semantics.
 */
extern "C" {
void uml_x86_64_x87_from_f32(CpuState *cpu, std::uint32_t bits,
	X87Register *result) noexcept;
void uml_x86_64_x87_from_f64(CpuState *cpu, std::uint64_t bits,
	X87Register *result) noexcept;
void uml_x86_64_x87_from_i32(std::int32_t value,
	X87Register *result) noexcept;
void uml_x86_64_x87_from_i64(std::int64_t value,
	X87Register *result) noexcept;
void uml_x86_64_x87_from_bcd(CpuState *cpu, const void *source,
	X87Register *result) noexcept;

std::uint32_t uml_x86_64_x87_to_f32(CpuState *cpu,
	const X87Register *value) noexcept;
std::uint64_t uml_x86_64_x87_to_f64(CpuState *cpu,
	const X87Register *value) noexcept;
std::uint32_t uml_x86_64_x87_to_i16(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept;
std::uint32_t uml_x86_64_x87_to_i32(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept;
std::uint64_t uml_x86_64_x87_to_i64(CpuState *cpu,
	const X87Register *value, std::uint32_t truncate) noexcept;
void uml_x86_64_x87_to_bcd(CpuState *cpu, const X87Register *value,
	void *destination) noexcept;

#define UML_X86_64_DECLARE_X87_BINARY(name) \
	void uml_x86_64_x87_##name(CpuState *cpu, const X87Register *left, \
		const X87Register *right, X87Register *result) noexcept;
UML_X86_64_DECLARE_X87_BINARY(add)
UML_X86_64_DECLARE_X87_BINARY(multiply)
UML_X86_64_DECLARE_X87_BINARY(subtract)
UML_X86_64_DECLARE_X87_BINARY(divide)
#undef UML_X86_64_DECLARE_X87_BINARY

/* FUCOM and FUCOMI set bit 0 of `flags`, FCOMI and FUCOMI bit 1. */
std::uint32_t uml_x86_64_x87_compare(CpuState *cpu,
	const X87Register *left, const X87Register *right,
	std::uint32_t flags) noexcept;
void uml_x86_64_x87_f2xm1(CpuState *cpu, const X87Register *input,
	X87Register *result) noexcept;
void uml_x86_64_x87_fyl2x(CpuState *cpu, const X87Register *value,
	const X87Register *multiplier, X87Register *result) noexcept;
void uml_x86_64_x87_fyl2xp1(CpuState *cpu, const X87Register *value,
	const X87Register *multiplier, X87Register *result) noexcept;
void uml_x86_64_x87_tangent(CpuState *cpu, const X87Register *input,
	X87Register *result, X87Register *second) noexcept;
void uml_x86_64_x87_atan(CpuState *cpu, const X87Register *y,
	const X87Register *x, X87Register *result) noexcept;
void uml_x86_64_x87_extract(CpuState *cpu, const X87Register *input,
	X87Register *exponent_result, X87Register *significand_result) noexcept;
void uml_x86_64_x87_fprem(CpuState *cpu, const X87Register *dividend,
	const X87Register *divisor, X87Register *result) noexcept;
void uml_x86_64_x87_fprem1(CpuState *cpu, const X87Register *dividend,
	const X87Register *divisor, X87Register *result) noexcept;
void uml_x86_64_x87_sqrt(CpuState *cpu, const X87Register *input,
	X87Register *result) noexcept;
void uml_x86_64_x87_sincos(CpuState *cpu, const X87Register *input,
	X87Register *sine_result, X87Register *cosine_result) noexcept;
void uml_x86_64_x87_round(CpuState *cpu, const X87Register *input,
	X87Register *result) noexcept;
void uml_x86_64_x87_scale(CpuState *cpu, const X87Register *value,
	const X87Register *exponent, X87Register *result) noexcept;
void uml_x86_64_x87_sine(CpuState *cpu, const X87Register *input,
	X87Register *result) noexcept;
void uml_x86_64_x87_cosine(CpuState *cpu, const X87Register *input,
	X87Register *result) noexcept;
/* The tag word FNSTENV and FNSAVE store: each register that is not empty
 * tagged by what it holds, 00 valid, 01 zero or 10 special. */
std::uint32_t uml_x86_64_x87_image_tags(const CpuState *cpu) noexcept;
}

} // namespace uml::cpu::x86_64::gadgets
