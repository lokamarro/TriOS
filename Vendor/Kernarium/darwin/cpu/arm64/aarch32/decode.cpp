// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aarch32/decode.hpp"

#include <bit>
#include <string_view>

namespace uml::cpu::arm64::aarch32 {

namespace {

constexpr std::uint8_t kAlways = 14;

bool constraint_holds(const Constraint &constraint, std::uint32_t word) noexcept
{
	std::uint32_t value = 0;
	for (unsigned index = 0; index < 3 && constraint.width[index] != 0; ++index) {
		const unsigned width = constraint.width[index];
		value = value << width | (word >> constraint.low[index] & ((1U << width) - 1));
	}
	const bool equal = (value & constraint.keep) == constraint.want;
	return equal != constraint.negate;
}

bool row_matches(const Row &row, std::uint32_t word) noexcept
{
	if ((word & row.mask) != row.value)
		return false;
	for (unsigned index = 0; index < row.constraint_count; ++index)
		if (!constraint_holds(kConstraints[row.constraint_first + index], word))
			return false;
	return true;
}

/* The row of the isa's tree that matches the word, or null. */
const Row *find_row(Isa isa, std::uint32_t word) noexcept
{
	const Node *node = &kNodes[kTreeRoots[static_cast<std::size_t>(isa)]];
	while (node->bit >= 0)
		node = &kNodes[word >> node->bit & 1U ? node->second : node->first];
	for (unsigned index = 0; index < node->second; ++index) {
		const Row &row = kRows[kLeafRows[node->first + index]];
		if (row_matches(row, word))
			return &row;
	}
	return nullptr;
}

constexpr std::uint32_t field(std::uint32_t word, unsigned low, unsigned width) noexcept
{
	return word >> low & ((1U << width) - 1);
}

/* Resolve a coprocessor access to a register the profile has. The fields sit
 * at the same word positions in A32 and T32. */
SysReg coprocessor_register(Form form, std::uint32_t word) noexcept
{
	if (!(field(word, 8, 1) == 1))
		return SysReg::none; /* coprocessor 14 */
	const bool read = field(word, 20, 1) != 0;
	const unsigned opc1 = field(word, 21, 3);
	const unsigned crn = field(word, 16, 4);
	const unsigned crm = field(word, 0, 4);
	const unsigned opc2 = field(word, 5, 3);
	switch (form) {
	case Form::MRC_A1:
	case Form::MRC_T1:
		if (!read || opc1 != 0)
			return SysReg::none;
		if (crn == 13 && crm == 0 && opc2 == 3)
			return SysReg::tpidruro;
		if (crn == 13 && crm == 0 && opc2 == 2)
			return SysReg::tpidrurw_read;
		if (crn == 14 && crm == 0 && opc2 == 0)
			return SysReg::cntfrq;
		return SysReg::none;
	case Form::MCR_A1:
	case Form::MCR_T1:
		if (read || opc1 != 0)
			return SysReg::none;
		if (crn == 13 && crm == 0 && opc2 == 2)
			return SysReg::tpidrurw_write;
		if (crn == 7 && crm == 10 && opc2 == 5)
			return SysReg::cp15_dmb;
		if (crn == 7 && crm == 10 && opc2 == 4)
			return SysReg::cp15_dsb;
		if (crn == 7 && crm == 5 && opc2 == 4)
			return SysReg::cp15_isb;
		return SysReg::none;
	case Form::MRRC_A1:
	case Form::MRRC_T1:
		/* MRRC p15, 1, <Rt>, <Rt2>, c14 is CNTVCT. */
		if (read && field(word, 4, 4) == 1 && crm == 14)
			return SysReg::cntvct;
		return SysReg::none;
	default:
		return SysReg::none;
	}
}

void undefine(Instruction &result, Reason reason) noexcept
{
	result.status = Status::undefined;
	result.reason = reason;
}

/* The should-be bits of the hint spaces that the decode index leaves out: a
 * reserved hint whose SBO or SBZ bits differ is UNPREDICTABLE. */
bool reserved_hint_violated(Isa isa, std::uint32_t word) noexcept
{
	if (isa == Isa::a32)
		return (word >> 28) != 0xF && (word >> 20 & 0xFF) == 0x32 && (word >> 12 & 0xF) != 0xF;
	if (isa == Isa::t32)
		return (word >> 20) == 0xF3A
			&& ((word >> 16 & 0xF) != 0xF || (word >> 13 & 1) != 0 || (word >> 8 & 0xF) != 0);
	return false;
}

/* Everything past the row: the profile, the operand program and the checks
 * the table cannot make. */
void decode_form(Instruction &result, const Row &row, std::uint32_t word, ItState it,
	const Profile &profile) noexcept
{
	const FormInfo &info = kForms[row.form];
	result.form = static_cast<Form>(row.form);
	if (row.kind == RowKind::unpredictable) {
		undefine(result, Reason::unpredictable);
		return;
	}
	if (info.availability == Availability::undefined_at_el0) {
		undefine(result, Reason::profile);
		return;
	}
	if (info.feature != kNoFeature
		&& !profile.features.has(static_cast<Feature>(info.feature))) {
		undefine(result, Reason::feature);
		return;
	}
	if ((word & info.should_mask) != info.should_value) {
		undefine(result, Reason::unpredictable);
		return;
	}
	const Context ctx{it, result.isa != Isa::a32, profile.features};
	switch (run_program(info.program, word, ctx, result.operands)) {
	case Outcome::ok:
		break;
	case Outcome::nop:
		result.status = Status::nop;
		result.reason = Reason::none;
		return;
	case Outcome::unpredictable:
		undefine(result, Reason::unpredictable);
		return;
	case Outcome::feature:
		undefine(result, Reason::feature);
		return;
	case Outcome::undefined:
	case Outcome::see:
		undefine(result, Reason::unallocated);
		return;
	}
	switch (info.check) {
	case Check::none:
		break;
	case Check::coprocessor:
		result.sysreg = coprocessor_register(result.form, word);
		if (result.sysreg == SysReg::none) {
			undefine(result, Reason::profile);
			return;
		}
		break;
	case Check::apsr:
		/* Only the APSR is reachable at EL0, never an SPSR. */
		if (result.operands.read_spsr || result.operands.write_spsr) {
			undefine(result, Reason::profile);
			return;
		}
		/* An MSR whose x field is set would write PSTATE.E (the endianness the
		 * profile does not implement); the c field is ignored at EL0. */
		if (std::string_view(info.mnemonic) == "MSR"
			&& (field(word, result.isa == Isa::a32 ? 16 : 8, 4) & 2U) != 0) {
			undefine(result, Reason::profile);
			return;
		}
		break;
	case Check::fpscr:
		/* VMRS and VMSR reach FPSCR only; FPSID, MVFRn and FPEXC are
		 * privileged. */
		if (field(word, 16, 4) != 1) {
			undefine(result, Reason::profile);
			return;
		}
		break;
	}
	if (info.alu_exception_return && result.operands.d == 15 && result.operands.setflags) {
		undefine(result, Reason::unpredictable);
		return;
	}
	result.status = info.nop ? Status::nop : Status::ok;
	result.reason = Reason::none;
}

Instruction decode_row(Isa isa, std::uint32_t word, ItState it, const Profile &profile) noexcept
{
	Instruction result;
	result.isa = isa;
	result.length = isa == Isa::t16 ? 2 : 4;
	result.word = word;
	const bool thumb = isa != Isa::a32;
	result.in_it = thumb && it.active();
	result.last_in_it = thumb && it.last();
	result.next_it = result.in_it ? it.advance() : ItState{};
	const Row *row = find_row(isa, word);
	if (row == nullptr)
		return result; /* undefined, unallocated */
	if (row->kind == RowKind::reserved_hint) {
		result.form = Form::ReservedHint;
		if (reserved_hint_violated(isa, word)) {
			undefine(result, Reason::unpredictable);
		} else {
			result.status = Status::nop;
			result.reason = Reason::none;
		}
	} else {
		decode_form(result, *row, word, it, profile);
	}
	const FormInfo &info = result.info();
	if (info.conditional && result.status != Status::undefined) {
		if (info.cond_low != kNoCond)
			result.cond = static_cast<std::uint8_t>(field(word, info.cond_low, 4));
		else if (result.in_it)
			result.cond = static_cast<std::uint8_t>(it.cond());
		else
			result.cond = kAlways;
	}
	if (result.form == Form::IT_T1 && result.status == Status::ok) {
		const unsigned firstcond = field(word, 20, 4);
		const unsigned mask = field(word, 16, 4);
		result.next_it = ItState::from_it(firstcond, mask);
		result.it_length = static_cast<std::uint8_t>(4 - std::countr_zero(mask));
	}
	return result;
}

} // namespace

Instruction decode_a32(std::uint32_t word, const Profile &profile) noexcept
{
	return decode_row(Isa::a32, word, ItState{}, profile);
}

Instruction decode_thumb(std::uint32_t word, ItState it, const Profile &profile) noexcept
{
	const bool wide = thumb_length(static_cast<std::uint16_t>(word >> 16)) == 4;
	return decode_row(wide ? Isa::t32 : Isa::t16, wide ? word : word & 0xFFFF0000U, it, profile);
}

Form classify(Isa isa, std::uint32_t word) noexcept
{
	const Row *row = find_row(isa, word);
	if (row == nullptr)
		return Form::Undefined;
	return row->kind == RowKind::reserved_hint ? Form::ReservedHint
						   : static_cast<Form>(row->form);
}

} // namespace uml::cpu::arm64::aarch32
