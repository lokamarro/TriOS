// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

enum class ExecutionMode : std::uint8_t {
	long64,
	compat32,
};

[[nodiscard]] constexpr std::uint64_t normalize_instruction_pointer(
	ExecutionMode mode, std::uint64_t value) noexcept
{
	return mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(value) : value;
}

[[nodiscard]] constexpr std::uint64_t advance_instruction_pointer(
	ExecutionMode mode, std::uint64_t value, std::uint64_t amount) noexcept
{
	return normalize_instruction_pointer(mode, value + amount);
}

enum class Register : std::uint8_t {
	rax,
	rcx,
	rdx,
	rbx,
	rsp,
	rbp,
	rsi,
	rdi,
	r8,
	r9,
	r10,
	r11,
	r12,
	r13,
	r14,
	r15,
};

enum class DeferredFlagOperation : std::uint8_t {
	none,
	add,
	subtract,
	logical,
	increment,
	decrement,
	add_carry,
	subtract_borrow,
	/* The shifts record the original value, the masked nonzero count and
	 * the result; see the engine's shift_flags_body for their flags. */
	shift_left,
	shift_right,
	shift_arithmetic_right,
};

struct DeferredFlags final {
	std::uint64_t left{};
	std::uint64_t right{};
	std::uint64_t result{};
	DeferredFlagOperation operation{DeferredFlagOperation::none};
	std::uint8_t width{};
	std::uint8_t carry{};
	/* Set on an INC or DEC record whose preserved carry has not been settled:
	 * it is whatever the record in CpuState::carry_slot defines. */
	std::uint8_t carry_pending{};
	std::uint16_t override_mask{};
	std::uint16_t override_value{};
};

struct X87Register final {
	std::uint64_t significand{};
	std::uint16_t sign_exponent{};
};

/* CpuState::exception_checks: the gadgets check for x87 exceptions the
 * guest unmasked, and for SIMD ones while underflow, precision and a
 * denormal operand stay masked. The SIMD floating-point gadgets take their
 * exact path, in the cold section, while they check SIMD exceptions or
 * MXCSR.DAZ or FTZ is set, in every run, checked or not; the gadgets that
 * load MXCSR keep that bit current. */
inline constexpr std::uint8_t exception_check_x87 = 1U << 0U;
inline constexpr std::uint8_t exception_check_simd = 1U << 1U;
inline constexpr std::uint8_t exception_check_simd_exact = 1U << 2U;

/* The hidden part of a segment register: what x86 loads from a descriptor
 * with a selector and uses until the next load. */
struct SegmentCache final {
	/* An access through FS or GS in compatibility mode at offset o passes
	 * the engines' inline check when o - check_low <= check_span, unsigned;
	 * one that does not leaves for the runtime's exact check. */
	std::uint64_t check_low{};
	std::uint64_t check_span{};
	std::uint32_t base{};
	/* The last offset a segment that expands up admits, in bytes. */
	std::uint32_t limit{};
	/* Bits 40 to 55 of the descriptor: type, S, DPL, P, the limit's bits 19
	 * to 16, AVL, L, D/B and G. */
	std::uint16_t attributes{};
	/* Clear for a null selector, through which no access passes. */
	bool usable{};
	std::uint8_t reserved{};
	std::uint32_t padding{};
};

struct CpuState final {
	std::array<std::uint64_t, 16> registers{};
	std::array<std::array<std::byte, 32>, 16> vectors{};
	std::array<std::uint64_t, 8> mmx{};
	std::array<X87Register, 8> x87{};
	/* ES, CS, SS, DS, FS and GS, in that order, which is x86's numbering
	 * of the segment registers. */
	std::array<std::uint16_t, 6> segment_selectors{};
	/* Set while the runtime runs, alone, an instruction whose every segment
	 * access it has already checked: the gadget guarding a check the engine
	 * cannot make itself lets it run. */
	std::uint8_t segment_bypass{};
	/* The segment registers' descriptor caches leave no place for the
	 * address generation of a flat compatibility-mode task to add a base or
	 * check a limit through ES, SS or DS: `flat_segments` says so, and the
	 * runtime steps any other compat32 task (`segments.hpp`). */
	bool flat_segments{true};
	/* The three TLS entries of the GDT, 12 to 14 (selectors 0x63 to 0x73),
	 * as descriptors, zero where the task installed none. */
	std::array<std::uint64_t, 3> tls_descriptors{};
	std::uint64_t rip{};
	std::uint64_t rflags{2};
	/* Address generation indexes {segment_zero, fs_base, gs_base} with the
	 * stream descriptor's segment selector, and names segment_zero for an
	 * absent base or index register. It must always read as zero.
	 */
	std::uint64_t segment_zero{};
	std::uint64_t fs_base{};
	std::uint64_t gs_base{};
	std::uint32_t mxcsr{0x1f80};
	std::uint16_t x87_control{0x037f};
	std::uint16_t x87_status{};
	std::uint16_t x87_tag{0xffff};
	std::uint8_t x87_top{};
	/* The exceptions the gadgets check for themselves during a dispatch,
	 * the `exception_check_*` bits. The runtime sets it as each dispatch
	 * enters the engine, and for an instruction it runs checked keeps only
	 * the exact path that MXCSR.DAZ or FTZ asks for; it is not
	 * architectural state. */
	std::uint8_t exception_checks{};
	bool system_call_32bit{};
	ExecutionMode execution_mode{ExecutionMode::long64};
	/* PR_SET_TSC(PR_TSC_SIGSEGV): RDTSC and RDTSCP raise #GP. */
	bool timestamp_faults{};
	/* Set by a check `exception_checks` enables when the instruction running
	 * raised an exception the guest unmasked; the gadget leaves before it
	 * commits anything, and the runtime runs that instruction again,
	 * checked. */
	std::uint8_t exception_raised{};
	DeferredFlags deferred_flags{};
	/* The record an INC or DEC displaced without settling its carry. Only a
	 * pending INC or DEC record reads it, and only for the carry flag. */
	DeferredFlags carry_slot{};
	/* The task's LDT, 8192 descriptors that the guest kernel owns and changes
	 * with single aligned stores, or null while the task has none. */
	const std::uint64_t *ldt{};
	/* The descriptor cache of each segment register, numbered as
	 * `segment_selectors` are, which `segments.hpp` keeps. */
	std::array<SegmentCache, 6> segments{};

	[[nodiscard]] std::uint64_t &gpr(Register reg) noexcept
	{
		return registers[static_cast<std::uint8_t>(reg)];
	}

	[[nodiscard]] const std::uint64_t &gpr(Register reg) const noexcept
	{
		return registers[static_cast<std::uint8_t>(reg)];
	}
};

inline constexpr std::uint64_t rflags_carry = 1ULL << 0;
inline constexpr std::uint64_t rflags_reserved = 1ULL << 1;
inline constexpr std::uint64_t rflags_parity = 1ULL << 2;
inline constexpr std::uint64_t rflags_auxiliary = 1ULL << 4;
inline constexpr std::uint64_t rflags_zero = 1ULL << 6;
inline constexpr std::uint64_t rflags_sign = 1ULL << 7;
inline constexpr std::uint64_t rflags_trap = 1ULL << 8;
inline constexpr std::uint64_t rflags_interrupt = 1ULL << 9;
inline constexpr std::uint64_t rflags_direction = 1ULL << 10;
inline constexpr std::uint64_t rflags_overflow = 1ULL << 11;
inline constexpr std::uint64_t rflags_resume = 1ULL << 16;
inline constexpr std::uint64_t rflags_alignment_check = 1ULL << 18;

/* The MXCSR bits the engines test for denormal handling. */
inline constexpr std::uint32_t mxcsr_denormals_are_zero = 1U << 6;
inline constexpr std::uint32_t mxcsr_flush_to_zero = 1U << 15;

void materialize_flags(CpuState &cpu) noexcept;

} // namespace uml::cpu::x86_64
