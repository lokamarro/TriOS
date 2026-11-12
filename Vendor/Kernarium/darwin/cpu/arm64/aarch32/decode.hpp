// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_encodings.hpp"
#include "cpu/arm64/aarch32/decode_types.hpp"

#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/* The AArch32 decoder: one table, generated from Arm's AArch32 XML
 * (tests/tooling/arm64/generate_a32_encoding_table.py), decodes A32, T16 and
 * T32 words for both engines and the semantic library. It includes no engine
 * and holds no state; the caller owns the instruction stream and the IT
 * state. docs/a32-instruction-coverage.md lists the forms and the profile's
 * choices.
 *
 * A word is decoded against a Profile. What the profile does not implement is
 * not guessed at: it decodes as Status::undefined with the reason, and the
 * engine raises the illegal-instruction exit. UNPREDICTABLE encodings are
 * undefined as well. */

enum class Status : std::uint8_t {
	/* The instruction exists in the profile; execute it. */
	ok,
	/* The instruction exists and has no effect (a reserved hint, CPS at EL0
	 * and the hints of features the profile lacks). */
	nop,
	/* Raise the illegal-instruction exit. */
	undefined,
};

enum class Reason : std::uint8_t {
	none,
	/* No instruction owns the encoding. */
	unallocated,
	/* The encoding is allocated but UNPREDICTABLE. */
	unpredictable,
	/* An instruction the profile leaves out: privileged at EL0, obsolete, or
	 * a coprocessor access that is not one of the profile's. */
	profile,
	/* An instruction of a feature the profile lacks. */
	feature,
};

/* The system register or operation a permitted coprocessor access (MCR, MRC,
 * MRRC) names. The CP15 barriers execute only when the guest's CP15BEN is
 * set; the engine tests that when it runs them. */
enum class SysReg : std::uint8_t {
	none,
	tpidruro,
	tpidrurw_read,
	tpidrurw_write,
	cntfrq,
	cntvct,
	cp15_dsb,
	cp15_dmb,
	cp15_isb,
};

struct Instruction final {
	Form form = Form::Undefined;
	Status status = Status::undefined;
	Reason reason = Reason::unallocated;
	Isa isa = Isa::a32;
	/* Bytes the instruction occupies: 4 for A32 and T32, 2 for T16. */
	std::uint8_t length = 4;
	/* The condition the instruction executes under (0 EQ .. 14 AL): its own
	 * field, the IT block's condition, or 14. */
	std::uint8_t cond = 14;
	/* Thumb only: the instruction is in an IT block, and is its last. */
	bool in_it = false;
	bool last_in_it = false;
	SysReg sysreg = SysReg::none;
	/* For IT: the instructions the block holds (1 to 4). */
	std::uint8_t it_length = 0;
	/* The IT state after this instruction. The caller advances its own state
	 * with it and treats a block as running until it is zero. */
	ItState next_it;
	/* The word as decoded: A32 bits, or hw1 << 16 | hw2 (hw2 zero for T16). */
	std::uint32_t word = 0;
	/* What the form's Decode pseudocode computes, named for its variables (d,
	 * n, m, t, imm32, setflags, shift_t, registers, esize, ...). A bits(N)
	 * value is its bit pattern, imm32 the 32-bit pattern, and an integer its
	 * value. */
	Operands operands;

	/* The form's table entry: the XML encoding name and mnemonic, its level
	 * and the feature it needs, whether it is conditional, and static traits
	 * scanned from the XML's Execute pseudocode (loads, stores, exclusives,
	 * the kinds of PC write, barriers, hints, exceptions, and fp_state, a
	 * read or write of the S, D, Q or V registers or FPSCR). fp_state is
	 * exactly the XML's FP&SIMD section: an engine that loads the task's
	 * vectors lazily loads them for any block that holds such a form. */
	[[nodiscard]] const FormInfo &info() const noexcept
	{
		return kForms[static_cast<std::size_t>(form)];
	}
	[[nodiscard]] bool legal() const noexcept { return status != Status::undefined; }
};

/* A Thumb instruction is 32 bits when its first halfword starts 0b11101,
 * 0b11110 or 0b11111. */
[[nodiscard]] constexpr unsigned thumb_length(std::uint16_t hw1) noexcept
{
	return hw1 >= 0xE800 ? 4U : 2U;
}

/* Decode an A32 word. */
[[nodiscard]] Instruction decode_a32(std::uint32_t word, const Profile &profile) noexcept;

/* Decode a Thumb instruction given as hw1 << 16 | hw2 (hw2 is ignored for a
 * 16-bit instruction) in the IT state the caller holds. */
[[nodiscard]] Instruction decode_thumb(
	std::uint32_t word, ItState it, const Profile &profile) noexcept;

/* The form a word belongs to by the encoding index alone, with no profile,
 * feature or operand checks: the reference the tests compare against. */
[[nodiscard]] Form classify(Isa isa, std::uint32_t word) noexcept;

} // namespace uml::cpu::arm64::aarch32
