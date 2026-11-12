// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_builder.hpp"

#include "native_baked.hpp"
#include "native_symbols.hpp"
#include "cpu/x86_64/segments.hpp"
#include "cpu/x86_64/layout/assembly_offsets.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <utility>

namespace uml::cpu::x86_64::gadgets {
namespace {

using NativeEntry = void (*)();

[[nodiscard]] constexpr std::uint64_t next_instruction_pointer(
	const Gadget &gadget, ExecutionMode mode) noexcept
{
	return advance_instruction_pointer(
		mode, gadget.address, gadget.instruction.length);
}

/* A direct CALL's opcode and 32-bit displacement. */
constexpr unsigned direct_call_length = 5;

[[nodiscard]] constexpr std::uint64_t relative_target(
	const Gadget &gadget, ExecutionMode mode) noexcept
{
	return normalize_instruction_pointer(mode,
		next_instruction_pointer(gadget, mode) +
		gadget.instruction.source.immediate);
}

[[nodiscard]] std::uintptr_t entry_address(NativeEntry entry) noexcept
{
	return reinterpret_cast<std::uintptr_t>(entry);
}

/* Bit 63 of a descriptor or an offset word tells a gadget shared by the
 * legacy and VEX encodings which one it runs. */
[[nodiscard]] constexpr std::uintptr_t vex_tag(
	const Instruction &instruction) noexcept
{
	return static_cast<std::uintptr_t>(instruction.vex) << 63U;
}

/* The words of one block's stream, and what an instruction's words need to
 * know of the block they are in. An instruction that can fault or leave
 * reports its address, which its stream word does not carry: the word holds
 * the instruction's position in the block, its address less the block's
 * start, and the exits add the start back from the context's program_begin.
 * Every stream of a block takes the block's own start as that base -- a
 * budget-limited replay and a checked instruction included -- so
 * `program_begin` is the block's, not the first instruction's of the span
 * being built. A compatibility-mode position wraps at 4 GiB as the address
 * does. */
class StreamWords final {
public:
	StreamWords(std::vector<std::uintptr_t> &words, ExecutionMode mode,
		std::uint64_t program_begin,
		StreamKind kind = StreamKind::fragment) noexcept :
		words_{&words}, mode_{mode}, program_begin_{program_begin},
		kind_{kind}
	{
	}

	void push_back(std::uintptr_t word) { words_->push_back(word); }

	/* Whether the stream is a block's own, from which the runtime recovers
	 * an instruction by a stream word it left with. */
	[[nodiscard]] bool block() const noexcept
	{
		return kind_ == StreamKind::block;
	}

	/* The instruction's position in the block, or nothing when it lies
	 * farther in than the stream word's field reaches. */
	[[nodiscard]] std::optional<std::uintptr_t> position(
		const Gadget &gadget) const noexcept
	{
		const auto position = normalize_instruction_pointer(
			mode_, gadget.address - program_begin_);
		if (position >= (std::uint64_t{1} << UML_X86_64_STREAM_POSITION_BITS))
			return std::nullopt;
		return static_cast<std::uintptr_t>(position);
	}

	/* The word fields that name the instruction: its position and length. */
	[[nodiscard]] std::uintptr_t locator(const Gadget &gadget) const noexcept
	{
		return (*position(gadget) << UML_X86_64_STREAM_POSITION) |
			(static_cast<std::uintptr_t>(gadget.instruction.length) << 56U);
	}

private:
	std::vector<std::uintptr_t> *words_;
	ExecutionMode mode_;
	std::uint64_t program_begin_;
	StreamKind kind_;
};

/* CpuState offsets packed into one stream word, 16 bits apiece. */
[[nodiscard]] constexpr std::uintptr_t pack_offsets(std::uint16_t first,
	std::uint16_t second = 0, std::uint16_t third = 0) noexcept
{
	return first | (std::uintptr_t{second} << 16U) |
		(std::uintptr_t{third} << 32U);
}

[[nodiscard]] std::optional<std::uint16_t> register_offset(
	const Operand &operand) noexcept
{
	if (operand.kind != OperandKind::reg)
		return std::nullopt;
	const auto offset = static_cast<std::uint16_t>(
		operand.reg.index * sizeof(std::uint64_t) +
		(operand.reg.high_byte ? 1U : 0U));
	return offset;
}

[[nodiscard]] std::optional<std::uint16_t> vector_offset(
	const Operand &operand) noexcept
{
	if (operand.kind != OperandKind::vector_reg)
		return std::nullopt;
	return static_cast<std::uint16_t>(offsetof(CpuState, vectors) +
		operand.reg.index * sizeof(CpuState::vectors[0]));
}

[[nodiscard]] std::optional<std::uint16_t> mmx_offset(
	const Operand &operand) noexcept
{
	if (operand.kind != OperandKind::mmx_reg || operand.reg.index >= 8U)
		return std::nullopt;
	return static_cast<std::uint16_t>(offsetof(CpuState, mmx) +
		operand.reg.index * sizeof(CpuState::mmx[0]));
}

[[nodiscard]] std::optional<std::uint16_t> vector_state_offset(
	const Operand &operand) noexcept
{
	if (const auto vector = vector_offset(operand))
		return vector;
	return mmx_offset(operand);
}

#define UML_X86_DEFINE_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width, bool immediate) noexcept \
	{ \
		switch (width) { \
		case OperandWidth::byte: \
			return entry_address(immediate ? \
				native::uml_x86_64_gadget_##operation##_8_ri : \
				native::uml_x86_64_gadget_##operation##_8_rr); \
		case OperandWidth::word: \
			return entry_address(immediate ? \
				native::uml_x86_64_gadget_##operation##_16_ri : \
				native::uml_x86_64_gadget_##operation##_16_rr); \
		case OperandWidth::dword: \
			return entry_address(immediate ? \
				native::uml_x86_64_gadget_##operation##_32_ri : \
				native::uml_x86_64_gadget_##operation##_32_rr); \
		case OperandWidth::qword: \
			return entry_address(immediate ? \
				native::uml_x86_64_gadget_##operation##_64_ri : \
				native::uml_x86_64_gadget_##operation##_64_rr); \
		case OperandWidth::xmm: \
		case OperandWidth::ymm: \
			break; \
		} \
		return 0; \
	}

UML_X86_DEFINE_SELECTOR(move)
UML_X86_DEFINE_SELECTOR(add)
UML_X86_DEFINE_SELECTOR(sub)
UML_X86_DEFINE_SELECTOR(and)
UML_X86_DEFINE_SELECTOR(or)
UML_X86_DEFINE_SELECTOR(xor)
UML_X86_DEFINE_SELECTOR(cmp)
UML_X86_DEFINE_SELECTOR(test)
UML_X86_DEFINE_SELECTOR(adc)
UML_X86_DEFINE_SELECTOR(sbb)

#undef UML_X86_DEFINE_SELECTOR

[[nodiscard]] std::uintptr_t select_exchange(
	OperandWidth width) noexcept
{
	switch (width) {
	case OperandWidth::byte: return entry_address(
		native::uml_x86_64_gadget_exchange_8_rr);
	case OperandWidth::word: return entry_address(
		native::uml_x86_64_gadget_exchange_16_rr);
	case OperandWidth::dword: return entry_address(
		native::uml_x86_64_gadget_exchange_32_rr);
	case OperandWidth::qword: return entry_address(
		native::uml_x86_64_gadget_exchange_64_rr);
	default: return 0;
	}
}

#define UML_X86_DEFINE_UNARY_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width) noexcept \
	{ \
		switch (width) { \
		case OperandWidth::byte: return entry_address( \
			native::uml_x86_64_gadget_##operation##_8_r); \
		case OperandWidth::word: return entry_address( \
			native::uml_x86_64_gadget_##operation##_16_r); \
		case OperandWidth::dword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_32_r); \
		case OperandWidth::qword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_64_r); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_UNARY_SELECTOR(increment)
UML_X86_DEFINE_UNARY_SELECTOR(decrement)
UML_X86_DEFINE_UNARY_SELECTOR(negate)
UML_X86_DEFINE_UNARY_SELECTOR(not)

#undef UML_X86_DEFINE_UNARY_SELECTOR

#define UML_X86_DEFINE_MEMORY_UNARY_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_memory_##operation( \
		OperandWidth width) noexcept \
	{ \
		switch (width) { \
		case OperandWidth::byte: return entry_address( \
			native::uml_x86_64_gadget_##operation##_8_m); \
		case OperandWidth::word: return entry_address( \
			native::uml_x86_64_gadget_##operation##_16_m); \
		case OperandWidth::dword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_32_m); \
		case OperandWidth::qword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_64_m); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_MEMORY_UNARY_SELECTOR(increment)
UML_X86_DEFINE_MEMORY_UNARY_SELECTOR(decrement)
UML_X86_DEFINE_MEMORY_UNARY_SELECTOR(negate)
UML_X86_DEFINE_MEMORY_UNARY_SELECTOR(not)

#undef UML_X86_DEFINE_MEMORY_UNARY_SELECTOR

#define UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_memory_##operation##_locked( \
		OperandWidth width) noexcept \
	{ \
		switch (width) { \
		case OperandWidth::byte: return entry_address( \
			native::uml_x86_64_gadget_##operation##_8_m_locked); \
		case OperandWidth::word: return entry_address( \
			native::uml_x86_64_gadget_##operation##_16_m_locked); \
		case OperandWidth::dword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_32_m_locked); \
		case OperandWidth::qword: return entry_address( \
			native::uml_x86_64_gadget_##operation##_64_m_locked); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR(increment)
UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR(decrement)
UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR(negate)
UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR(not)

#undef UML_X86_DEFINE_LOCKED_MEMORY_UNARY_SELECTOR

[[nodiscard]] std::uintptr_t arithmetic_entry(
	Operation operation, OperandWidth width, bool immediate) noexcept
{
	switch (operation) {
	case Operation::add: return select_add(width, immediate);
	case Operation::subtract: return select_sub(width, immediate);
	case Operation::bit_and: return select_and(width, immediate);
	case Operation::bit_or: return select_or(width, immediate);
	case Operation::bit_xor: return select_xor(width, immediate);
	case Operation::compare: return select_cmp(width, immediate);
	case Operation::test: return select_test(width, immediate);
	default: return 0;
	}
}

enum class MemoryMoveForm {
	load,
	store_register,
	store_immediate,
};

enum class MemoryArithmeticForm {
	register_memory,
	memory_register,
	memory_immediate,
};

enum class ShiftForm {
	register_register,
	register_immediate,
	memory_register,
	memory_immediate,
};

enum class DoubleShiftForm {
	register_immediate,
	register_register,
	memory_immediate,
	memory_register,
};

enum class MultiplyForm {
	implicit_register,
	implicit_memory,
	explicit_register,
	explicit_memory,
	immediate_register,
	immediate_memory,
};

enum class BmiKind {
	blsr,
	blsmsk,
	blsi,
	andn,
	bzhi,
	bextr,
	shlx,
	sarx,
	shrx,
	pdep,
	pext,
	mulx,
};

#define UML_X86_DEFINE_SHIFT_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width, ShiftForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry rr, NativeEntry ri, \
			NativeEntry mr, NativeEntry mi) { \
			switch (form) { \
			case ShiftForm::register_register: return entry_address(rr); \
			case ShiftForm::register_immediate: return entry_address(ri); \
			case ShiftForm::memory_register: return entry_address(mr); \
			case ShiftForm::memory_immediate: return entry_address(mi); \
			} \
			return std::uintptr_t{}; \
		}; \
		switch (width) { \
		case OperandWidth::byte: return select( \
			native::uml_x86_64_gadget_##operation##_8_rr, \
			native::uml_x86_64_gadget_##operation##_8_ri, \
			native::uml_x86_64_gadget_##operation##_8_mr, \
			native::uml_x86_64_gadget_##operation##_8_mi); \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_rr, \
			native::uml_x86_64_gadget_##operation##_16_ri, \
			native::uml_x86_64_gadget_##operation##_16_mr, \
			native::uml_x86_64_gadget_##operation##_16_mi); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_rr, \
			native::uml_x86_64_gadget_##operation##_32_ri, \
			native::uml_x86_64_gadget_##operation##_32_mr, \
			native::uml_x86_64_gadget_##operation##_32_mi); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_rr, \
			native::uml_x86_64_gadget_##operation##_64_ri, \
			native::uml_x86_64_gadget_##operation##_64_mr, \
			native::uml_x86_64_gadget_##operation##_64_mi); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_SHIFT_SELECTOR(shl)
UML_X86_DEFINE_SHIFT_SELECTOR(shr)
UML_X86_DEFINE_SHIFT_SELECTOR(sar)
UML_X86_DEFINE_SHIFT_SELECTOR(rol)
UML_X86_DEFINE_SHIFT_SELECTOR(ror)
UML_X86_DEFINE_SHIFT_SELECTOR(rcl)
UML_X86_DEFINE_SHIFT_SELECTOR(rcr)

#undef UML_X86_DEFINE_SHIFT_SELECTOR

[[nodiscard]] std::uintptr_t shift_entry(Operation operation,
	OperandWidth width, ShiftForm form) noexcept
{
	switch (operation) {
	case Operation::shift_left: return select_shl(width, form);
	case Operation::shift_right: return select_shr(width, form);
	case Operation::shift_arithmetic_right: return select_sar(width, form);
	case Operation::rotate_left: return select_rol(width, form);
	case Operation::rotate_right: return select_ror(width, form);
	case Operation::rotate_carry_left: return select_rcl(width, form);
	case Operation::rotate_carry_right: return select_rcr(width, form);
	default: return 0;
	}
}

#define UML_X86_DEFINE_DOUBLE_SHIFT_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width, DoubleShiftForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry rri, NativeEntry rrr, \
			NativeEntry mri, NativeEntry mrr) { \
			switch (form) { \
			case DoubleShiftForm::register_immediate: return entry_address(rri); \
			case DoubleShiftForm::register_register: return entry_address(rrr); \
			case DoubleShiftForm::memory_immediate: return entry_address(mri); \
			case DoubleShiftForm::memory_register: return entry_address(mrr); \
			} \
			return std::uintptr_t{}; \
		}; \
		switch (width) { \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_rri, \
			native::uml_x86_64_gadget_##operation##_16_rrr, \
			native::uml_x86_64_gadget_##operation##_16_mri, \
			native::uml_x86_64_gadget_##operation##_16_mrr); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_rri, \
			native::uml_x86_64_gadget_##operation##_32_rrr, \
			native::uml_x86_64_gadget_##operation##_32_mri, \
			native::uml_x86_64_gadget_##operation##_32_mrr); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_rri, \
			native::uml_x86_64_gadget_##operation##_64_rrr, \
			native::uml_x86_64_gadget_##operation##_64_mri, \
			native::uml_x86_64_gadget_##operation##_64_mrr); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_DOUBLE_SHIFT_SELECTOR(shld)
UML_X86_DEFINE_DOUBLE_SHIFT_SELECTOR(shrd)

#undef UML_X86_DEFINE_DOUBLE_SHIFT_SELECTOR

[[nodiscard]] std::uintptr_t double_shift_entry(Operation operation,
	OperandWidth width, DoubleShiftForm form) noexcept
{
	return operation == Operation::double_shift_left ?
		select_shld(width, form) : operation == Operation::double_shift_right ?
		select_shrd(width, form) : 0;
}

[[nodiscard]] std::uintptr_t multiply_entry(bool sign, OperandWidth width,
	MultiplyForm form) noexcept
{
#define UML_X86_MULTIPLY_IMPLICIT(width_bits) \
	if (form == MultiplyForm::implicit_register) \
		return entry_address(sign ? \
			native::uml_x86_64_gadget_imul_##width_bits##_r : \
			native::uml_x86_64_gadget_mul_##width_bits##_r); \
	if (form == MultiplyForm::implicit_memory) \
		return entry_address(sign ? \
			native::uml_x86_64_gadget_imul_##width_bits##_m : \
			native::uml_x86_64_gadget_mul_##width_bits##_m)
#define UML_X86_MULTIPLY_EXPLICIT(width_bits) \
	if (form == MultiplyForm::explicit_register) \
		return entry_address(native::uml_x86_64_gadget_imul_##width_bits##_rr); \
	if (form == MultiplyForm::explicit_memory) \
		return entry_address(native::uml_x86_64_gadget_imul_##width_bits##_rmr); \
	if (form == MultiplyForm::immediate_register) \
		return entry_address(native::uml_x86_64_gadget_imul_##width_bits##_rri); \
	if (form == MultiplyForm::immediate_memory) \
		return entry_address(native::uml_x86_64_gadget_imul_##width_bits##_rmi)
	switch (width) {
	case OperandWidth::byte:
		UML_X86_MULTIPLY_IMPLICIT(8);
		break;
	case OperandWidth::word:
		UML_X86_MULTIPLY_IMPLICIT(16);
		if (sign) {
			UML_X86_MULTIPLY_EXPLICIT(16);
		}
		break;
	case OperandWidth::dword:
		UML_X86_MULTIPLY_IMPLICIT(32);
		if (sign) {
			UML_X86_MULTIPLY_EXPLICIT(32);
		}
		break;
	case OperandWidth::qword:
		UML_X86_MULTIPLY_IMPLICIT(64);
		if (sign) {
			UML_X86_MULTIPLY_EXPLICIT(64);
		}
		break;
	default: break;
	}
#undef UML_X86_MULTIPLY_EXPLICIT
#undef UML_X86_MULTIPLY_IMPLICIT
	return 0;
}

[[nodiscard]] std::uintptr_t divide_entry(bool sign, OperandWidth width,
	bool memory) noexcept
{
#define UML_X86_DIVIDE_ENTRY(operation, width_bits) \
	return entry_address(memory ? \
		native::uml_x86_64_gadget_##operation##_##width_bits##_m : \
		native::uml_x86_64_gadget_##operation##_##width_bits##_r)
	switch (width) {
	case OperandWidth::byte:
		if (sign) UML_X86_DIVIDE_ENTRY(idiv, 8);
		UML_X86_DIVIDE_ENTRY(div, 8);
	case OperandWidth::word:
		if (sign) UML_X86_DIVIDE_ENTRY(idiv, 16);
		UML_X86_DIVIDE_ENTRY(div, 16);
	case OperandWidth::dword:
		if (sign) UML_X86_DIVIDE_ENTRY(idiv, 32);
		UML_X86_DIVIDE_ENTRY(div, 32);
	case OperandWidth::qword:
		if (sign) UML_X86_DIVIDE_ENTRY(idiv, 64);
		UML_X86_DIVIDE_ENTRY(div, 64);
	default: break;
	}
#undef UML_X86_DIVIDE_ENTRY
	return 0;
}

#define UML_X86_DEFINE_BIT_OPERATION_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width, ShiftForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry rr, NativeEntry ri, \
			NativeEntry mr, NativeEntry mi) { \
			switch (form) { \
			case ShiftForm::register_register: return entry_address(rr); \
			case ShiftForm::register_immediate: return entry_address(ri); \
			case ShiftForm::memory_register: return entry_address(mr); \
			case ShiftForm::memory_immediate: return entry_address(mi); \
			} \
			return std::uintptr_t{}; \
		}; \
		switch (width) { \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_rr, \
			native::uml_x86_64_gadget_##operation##_16_ri, \
			native::uml_x86_64_gadget_##operation##_16_mr, \
			native::uml_x86_64_gadget_##operation##_16_mi); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_rr, \
			native::uml_x86_64_gadget_##operation##_32_ri, \
			native::uml_x86_64_gadget_##operation##_32_mr, \
			native::uml_x86_64_gadget_##operation##_32_mi); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_rr, \
			native::uml_x86_64_gadget_##operation##_64_ri, \
			native::uml_x86_64_gadget_##operation##_64_mr, \
			native::uml_x86_64_gadget_##operation##_64_mi); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_BIT_OPERATION_SELECTOR(bt)
UML_X86_DEFINE_BIT_OPERATION_SELECTOR(bts)
UML_X86_DEFINE_BIT_OPERATION_SELECTOR(btr)
UML_X86_DEFINE_BIT_OPERATION_SELECTOR(btc)

#undef UML_X86_DEFINE_BIT_OPERATION_SELECTOR

/* A LOCK prefix needs the memory destination form, which updates the operand
 * with an exclusive loop instead of a plain load and store.
 */
#define UML_X86_DEFINE_LOCKED_BIT_OPERATION_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation##_locked( \
		OperandWidth width, ShiftForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry mr, NativeEntry mi) { \
			return entry_address(form == ShiftForm::memory_register ? \
				mr : mi); \
		}; \
		switch (width) { \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_mr_locked, \
			native::uml_x86_64_gadget_##operation##_16_mi_locked); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_mr_locked, \
			native::uml_x86_64_gadget_##operation##_32_mi_locked); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_mr_locked, \
			native::uml_x86_64_gadget_##operation##_64_mi_locked); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_LOCKED_BIT_OPERATION_SELECTOR(bts)
UML_X86_DEFINE_LOCKED_BIT_OPERATION_SELECTOR(btr)
UML_X86_DEFINE_LOCKED_BIT_OPERATION_SELECTOR(btc)

#undef UML_X86_DEFINE_LOCKED_BIT_OPERATION_SELECTOR

[[nodiscard]] std::uintptr_t bit_operation_entry(Operation operation,
	OperandWidth width, ShiftForm form, bool locked) noexcept
{
	if (locked) {
		/* BT only reads, so it cannot carry the prefix, and a register
		 * destination is not shared memory.
		 */
		if (form != ShiftForm::memory_register &&
			form != ShiftForm::memory_immediate)
			return 0;
		switch (operation) {
		case Operation::bit_set: return select_bts_locked(width, form);
		case Operation::bit_reset: return select_btr_locked(width, form);
		case Operation::bit_complement:
			return select_btc_locked(width, form);
		default: return 0;
		}
	}
	switch (operation) {
	case Operation::bit_test: return select_bt(width, form);
	case Operation::bit_set: return select_bts(width, form);
	case Operation::bit_reset: return select_btr(width, form);
	case Operation::bit_complement: return select_btc(width, form);
	default: return 0;
	}
}

#define UML_X86_DEFINE_BIT_COUNT_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_##operation( \
		OperandWidth width, bool memory) noexcept \
	{ \
		switch (width) { \
		case OperandWidth::word: return entry_address(memory ? \
			native::uml_x86_64_gadget_##operation##_16_rm : \
			native::uml_x86_64_gadget_##operation##_16_rr); \
		case OperandWidth::dword: return entry_address(memory ? \
			native::uml_x86_64_gadget_##operation##_32_rm : \
			native::uml_x86_64_gadget_##operation##_32_rr); \
		case OperandWidth::qword: return entry_address(memory ? \
			native::uml_x86_64_gadget_##operation##_64_rm : \
			native::uml_x86_64_gadget_##operation##_64_rr); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_BIT_COUNT_SELECTOR(bsf)
UML_X86_DEFINE_BIT_COUNT_SELECTOR(bsr)
UML_X86_DEFINE_BIT_COUNT_SELECTOR(tzcnt)
UML_X86_DEFINE_BIT_COUNT_SELECTOR(lzcnt)
UML_X86_DEFINE_BIT_COUNT_SELECTOR(popcnt)

#undef UML_X86_DEFINE_BIT_COUNT_SELECTOR

[[nodiscard]] std::uintptr_t bit_count_entry(Operation operation,
	OperandWidth width, bool memory) noexcept
{
	switch (operation) {
	case Operation::bit_scan_forward: return select_bsf(width, memory);
	case Operation::bit_scan_reverse: return select_bsr(width, memory);
	case Operation::count_trailing_zeros: return select_tzcnt(width, memory);
	case Operation::count_leading_zeros: return select_lzcnt(width, memory);
	case Operation::population_count: return select_popcnt(width, memory);
	default: return 0;
	}
}

[[nodiscard]] std::uintptr_t atomic_binary_entry(Operation operation,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_ATOMIC_ENTRY(name, width_bits) \
	return entry_address(memory ? \
		native::uml_x86_64_gadget_##name##_##width_bits##_mr : \
		native::uml_x86_64_gadget_##name##_##width_bits##_rr)
	switch (operation) {
	case Operation::compare_exchange:
		switch (width) {
		case OperandWidth::byte: UML_X86_ATOMIC_ENTRY(cmpxchg, 8);
		case OperandWidth::word: UML_X86_ATOMIC_ENTRY(cmpxchg, 16);
		case OperandWidth::dword: UML_X86_ATOMIC_ENTRY(cmpxchg, 32);
		case OperandWidth::qword: UML_X86_ATOMIC_ENTRY(cmpxchg, 64);
		default: return 0;
		}
	case Operation::exchange_add:
		switch (width) {
		case OperandWidth::byte: UML_X86_ATOMIC_ENTRY(xadd, 8);
		case OperandWidth::word: UML_X86_ATOMIC_ENTRY(xadd, 16);
		case OperandWidth::dword: UML_X86_ATOMIC_ENTRY(xadd, 32);
		case OperandWidth::qword: UML_X86_ATOMIC_ENTRY(xadd, 64);
		default: return 0;
		}
	default: return 0;
	}
#undef UML_X86_ATOMIC_ENTRY
}

[[nodiscard]] std::uintptr_t movbe_entry(
	OperandWidth width, bool load) noexcept
{
#define UML_X86_MOVBE_ENTRY(width_bits) \
	return entry_address(load ? \
		native::uml_x86_64_gadget_movbe_##width_bits##_rm : \
		native::uml_x86_64_gadget_movbe_##width_bits##_mr)
	switch (width) {
	case OperandWidth::word: UML_X86_MOVBE_ENTRY(16);
	case OperandWidth::dword: UML_X86_MOVBE_ENTRY(32);
	case OperandWidth::qword: UML_X86_MOVBE_ENTRY(64);
	default: return 0;
	}
#undef UML_X86_MOVBE_ENTRY
}

[[nodiscard]] std::uintptr_t crc32_entry(
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_CRC32_ENTRY(width_bits) \
	return entry_address(memory ? \
		native::uml_x86_64_gadget_crc32_##width_bits##_rm : \
		native::uml_x86_64_gadget_crc32_##width_bits##_rr)
	switch (width) {
	case OperandWidth::byte: UML_X86_CRC32_ENTRY(8);
	case OperandWidth::word: UML_X86_CRC32_ENTRY(16);
	case OperandWidth::dword: UML_X86_CRC32_ENTRY(32);
	case OperandWidth::qword: UML_X86_CRC32_ENTRY(64);
	default: return 0;
	}
#undef UML_X86_CRC32_ENTRY
}

[[nodiscard]] std::uintptr_t adx_entry(bool overflow, OperandWidth width,
	bool memory) noexcept
{
#define UML_X86_ADX_ENTRY(operation, width_bits) \
	return entry_address(memory ? \
		native::uml_x86_64_gadget_##operation##_##width_bits##_rm : \
		native::uml_x86_64_gadget_##operation##_##width_bits##_rr)
	switch (width) {
	case OperandWidth::dword:
		if (overflow) UML_X86_ADX_ENTRY(adox, 32);
		UML_X86_ADX_ENTRY(adcx, 32);
	case OperandWidth::qword:
		if (overflow) UML_X86_ADX_ENTRY(adox, 64);
		UML_X86_ADX_ENTRY(adcx, 64);
	default: return 0;
	}
#undef UML_X86_ADX_ENTRY
}

[[nodiscard]] std::uintptr_t bmi_unary_entry(BmiKind kind,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_BMI_UNARY_CASE(operation) \
	case BmiKind::operation: \
		if (width == OperandWidth::dword) \
			return entry_address(memory ? \
				native::uml_x86_64_gadget_##operation##_32_rm : \
				native::uml_x86_64_gadget_##operation##_32_rr); \
		if (width == OperandWidth::qword) \
			return entry_address(memory ? \
				native::uml_x86_64_gadget_##operation##_64_rm : \
				native::uml_x86_64_gadget_##operation##_64_rr); \
		return 0
	switch (kind) {
	UML_X86_BMI_UNARY_CASE(blsr);
	UML_X86_BMI_UNARY_CASE(blsmsk);
	UML_X86_BMI_UNARY_CASE(blsi);
	default: return 0;
	}
#undef UML_X86_BMI_UNARY_CASE
}

[[nodiscard]] std::uintptr_t bmi_ternary_entry(BmiKind kind,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_BMI_TERNARY_CASE(operation) \
	case BmiKind::operation: \
		if (width == OperandWidth::dword) \
			return entry_address(memory ? \
				native::uml_x86_64_gadget_##operation##_32_rrm : \
				native::uml_x86_64_gadget_##operation##_32_rrr); \
		if (width == OperandWidth::qword) \
			return entry_address(memory ? \
				native::uml_x86_64_gadget_##operation##_64_rrm : \
				native::uml_x86_64_gadget_##operation##_64_rrr); \
		return 0
	switch (kind) {
	UML_X86_BMI_TERNARY_CASE(andn);
	UML_X86_BMI_TERNARY_CASE(bzhi);
	UML_X86_BMI_TERNARY_CASE(bextr);
	UML_X86_BMI_TERNARY_CASE(shlx);
	UML_X86_BMI_TERNARY_CASE(sarx);
	UML_X86_BMI_TERNARY_CASE(shrx);
	UML_X86_BMI_TERNARY_CASE(pdep);
	UML_X86_BMI_TERNARY_CASE(pext);
	UML_X86_BMI_TERNARY_CASE(mulx);
	default: return 0;
	}
#undef UML_X86_BMI_TERNARY_CASE
}

[[nodiscard]] std::uintptr_t rorx_entry(
	OperandWidth width, bool memory) noexcept
{
	if (width == OperandWidth::dword)
		return entry_address(memory ?
			native::uml_x86_64_gadget_rorx_32_rmi :
			native::uml_x86_64_gadget_rorx_32_rri);
	if (width == OperandWidth::qword)
		return entry_address(memory ?
			native::uml_x86_64_gadget_rorx_64_rmi :
			native::uml_x86_64_gadget_rorx_64_rri);
	return 0;
}

#define UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_memory_##operation( \
		OperandWidth width, MemoryArithmeticForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry rm, NativeEntry mr, \
			NativeEntry mi) { \
			return entry_address(form == \
				MemoryArithmeticForm::register_memory ? rm : form == \
				MemoryArithmeticForm::memory_register ? mr : mi); \
		}; \
		switch (width) { \
		case OperandWidth::byte: return select( \
			native::uml_x86_64_gadget_##operation##_8_rm, \
			native::uml_x86_64_gadget_##operation##_8_mr, \
			native::uml_x86_64_gadget_##operation##_8_mi); \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_rm, \
			native::uml_x86_64_gadget_##operation##_16_mr, \
			native::uml_x86_64_gadget_##operation##_16_mi); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_rm, \
			native::uml_x86_64_gadget_##operation##_32_mr, \
			native::uml_x86_64_gadget_##operation##_32_mi); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_rm, \
			native::uml_x86_64_gadget_##operation##_64_mr, \
			native::uml_x86_64_gadget_##operation##_64_mi); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(add)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(sub)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(and)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(or)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(xor)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(cmp)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(test)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(adc)
UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR(sbb)

#undef UML_X86_DEFINE_MEMORY_ARITHMETIC_SELECTOR

/* Only the memory-destination forms write the operand, so only they can carry
 * a LOCK prefix and need the exclusive-loop gadget.
 */
#define UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(operation) \
	[[nodiscard]] std::uintptr_t select_memory_##operation##_locked( \
		OperandWidth width, MemoryArithmeticForm form) noexcept \
	{ \
		const auto select = [form](NativeEntry mr, NativeEntry mi) { \
			return entry_address(form == \
				MemoryArithmeticForm::memory_register ? mr : mi); \
		}; \
		switch (width) { \
		case OperandWidth::byte: return select( \
			native::uml_x86_64_gadget_##operation##_8_mr_locked, \
			native::uml_x86_64_gadget_##operation##_8_mi_locked); \
		case OperandWidth::word: return select( \
			native::uml_x86_64_gadget_##operation##_16_mr_locked, \
			native::uml_x86_64_gadget_##operation##_16_mi_locked); \
		case OperandWidth::dword: return select( \
			native::uml_x86_64_gadget_##operation##_32_mr_locked, \
			native::uml_x86_64_gadget_##operation##_32_mi_locked); \
		case OperandWidth::qword: return select( \
			native::uml_x86_64_gadget_##operation##_64_mr_locked, \
			native::uml_x86_64_gadget_##operation##_64_mi_locked); \
		default: return 0; \
		} \
	}

UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(add)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(sub)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(and)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(or)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(xor)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(adc)
UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR(sbb)

#undef UML_X86_DEFINE_LOCKED_MEMORY_ARITHMETIC_SELECTOR

[[nodiscard]] std::uintptr_t memory_arithmetic_entry(Operation operation,
	OperandWidth width, MemoryArithmeticForm form, bool locked) noexcept
{
	if (locked) {
		/* The register-destination form leaves memory unmodified, so it
		 * cannot carry the prefix.
		 */
		if (form == MemoryArithmeticForm::register_memory)
			return 0;
		switch (operation) {
		case Operation::add:
			return select_memory_add_locked(width, form);
		case Operation::subtract:
			return select_memory_sub_locked(width, form);
		case Operation::bit_and:
			return select_memory_and_locked(width, form);
		case Operation::bit_or:
			return select_memory_or_locked(width, form);
		case Operation::bit_xor:
			return select_memory_xor_locked(width, form);
		case Operation::add_carry:
			return select_memory_adc_locked(width, form);
		case Operation::subtract_borrow:
			return select_memory_sbb_locked(width, form);
		default: return 0;
		}
	}
	switch (operation) {
	case Operation::add: return select_memory_add(width, form);
	case Operation::subtract: return select_memory_sub(width, form);
	case Operation::bit_and: return select_memory_and(width, form);
	case Operation::bit_or: return select_memory_or(width, form);
	case Operation::bit_xor: return select_memory_xor(width, form);
	case Operation::compare: return select_memory_cmp(width, form);
	case Operation::test: return select_memory_test(width, form);
	case Operation::add_carry: return select_memory_adc(width, form);
	case Operation::subtract_borrow: return select_memory_sbb(width, form);
	default: return 0;
	}
}

[[nodiscard]] std::uintptr_t memory_move_entry(
	OperandWidth width, MemoryMoveForm form) noexcept
{
#define UML_X86_MEMORY_MOVE(width_bits) \
	switch (form) { \
	case MemoryMoveForm::load: return entry_address( \
		native::uml_x86_64_gadget_move_##width_bits##_rm); \
	case MemoryMoveForm::store_register: return entry_address( \
		native::uml_x86_64_gadget_move_##width_bits##_mr); \
	case MemoryMoveForm::store_immediate: return entry_address( \
		native::uml_x86_64_gadget_move_##width_bits##_mi); \
	}
	switch (width) {
	case OperandWidth::byte: UML_X86_MEMORY_MOVE(8);
	case OperandWidth::word: UML_X86_MEMORY_MOVE(16);
	case OperandWidth::dword: UML_X86_MEMORY_MOVE(32);
	case OperandWidth::qword: UML_X86_MEMORY_MOVE(64);
	default: return 0;
	}
#undef UML_X86_MEMORY_MOVE
}

struct EncodedMemory final {
	std::uintptr_t descriptor{};
	std::uintptr_t displacement{};
};

/* A memory descriptor names the value, base and index registers by ten-bit
 * CpuState offsets, which the sixteen general registers, the vector and x87
 * registers and the always-zero slot all lie below. */
constexpr std::uintptr_t descriptor_offset_limit = 1024;
static_assert(UML_X86_64_CPU_SEGMENT_ZERO < descriptor_offset_limit);
static_assert(UML_X86_64_CPU_SEGMENT_SELECTORS + 6 * 2 <=
	descriptor_offset_limit);

enum class DescriptorTag { none, vex };

/* The three words every memory form's stream starts with: its entry, the
 * operand descriptor, tagged for a VEX form and locating the instruction in
 * its block, and the displacement. It is always inlined, and each push takes
 * its word the way the callers' own pushes did: the entry by its value
 * category, the rest from where they live. The callers' code then stays
 * within a few instructions of the open-coded pushes. */
template<typename Entry>
[[gnu::always_inline]] inline void append_memory_words(
	StreamWords &stream, Entry &&entry,
	const EncodedMemory &encoded, const Gadget &gadget,
	DescriptorTag tag = DescriptorTag::none)
{
	stream.push_back(std::forward<Entry>(entry));
	if (tag == DescriptorTag::vex)
		stream.push_back(encoded.descriptor | stream.locator(gadget) |
			vex_tag(gadget.instruction));
	else
		stream.push_back(encoded.descriptor | stream.locator(gadget));
	stream.push_back(encoded.displacement);
}

[[nodiscard]] std::optional<EncodedMemory> encode_memory(
	const Gadget &gadget, const Operand &operand,
	std::uint16_t value_offset, bool apply_segment = true) noexcept
{
	if (operand.kind != OperandKind::memory || operand.memory.vector_index)
		return std::nullopt;
	const auto &memory = operand.memory;
	/* An absent base or index names the always-zero CpuState slot, so address
	 * generation adds it unconditionally instead of branching on a sentinel.
	 */
	const auto base = memory.base == MemoryOperand::no_register ?
		unsigned{UML_X86_64_CPU_SEGMENT_ZERO} :
		static_cast<unsigned>(memory.base * sizeof(std::uint64_t));
	const auto index = memory.index == MemoryOperand::no_register ?
		unsigned{UML_X86_64_CPU_SEGMENT_ZERO} :
		static_cast<unsigned>(memory.index * sizeof(std::uint64_t));
	const auto scale_shift = std::countr_zero(
		static_cast<unsigned>(memory.scale));
	const auto segment = apply_segment ? memory.segment : Segment::none;
	/* A stepped instruction's ES, CS, SS or DS names that segment, whose base
	 * the general tail adds. */
	const bool legacy = apply_segment && memory.segment_base &&
		segment == Segment::none;
	const auto segment_field = legacy ?
		static_cast<std::uintptr_t>(operand_segment(memory)) :
		static_cast<std::uintptr_t>(segment);
	/* Nothing follows the sum of base, scaled index and displacement: no
	 * truncation to a narrow address space and no segment base to add. The
	 * gadget takes that as permission to stop there, so it is set only when
	 * every one of the three is absent.
	 */
	const auto plain = segment == Segment::none && !legacy &&
		!memory.address_32 && !memory.address_16;
	/* The three offsets are ten-bit fields; every CpuState offset a gadget
	 * names is below 1024, but a value offset comes from the caller. */
	if (value_offset >= descriptor_offset_limit)
		return std::nullopt;
	std::uintptr_t descriptor = value_offset |
		(static_cast<std::uintptr_t>(base) << 16U) |
		(static_cast<std::uintptr_t>(index) << 26U) |
		(static_cast<std::uintptr_t>(scale_shift) << 48U) |
		(segment_field << 50U) |
		(static_cast<std::uintptr_t>(legacy) <<
			UML_X86_64_MEMORY_LEGACY_SEGMENT) |
		(static_cast<std::uintptr_t>(memory.address_32) << 52U) |
		(static_cast<std::uintptr_t>(memory.address_16) << 53U) |
		(static_cast<std::uintptr_t>(plain) <<
			UML_X86_64_MEMORY_PLAIN_ADDRESS) |
		(static_cast<std::uintptr_t>(gadget.instruction.length) << 56U);
	std::uintptr_t displacement{};
	constexpr auto register_fields =
		(std::uintptr_t{descriptor_offset_limit - 1U} << 16U) |
		(std::uintptr_t{descriptor_offset_limit - 1U} << 26U);
	constexpr auto zero_registers =
		(std::uintptr_t{UML_X86_64_CPU_SEGMENT_ZERO} << 16U) |
		(std::uintptr_t{UML_X86_64_CPU_SEGMENT_ZERO} << 26U);
	if (memory.absolute_address) {
		displacement = memory.absolute;
		descriptor = (descriptor & ~register_fields) | zero_registers;
	} else if (memory.rip_relative) {
		displacement = gadget.address + gadget.instruction.length +
			static_cast<std::int64_t>(memory.displacement);
		descriptor = (descriptor & ~register_fields) | zero_registers;
	} else {
		displacement = static_cast<std::uintptr_t>(
			static_cast<std::int64_t>(memory.displacement));
	}
	return EncodedMemory{descriptor, displacement};
}

[[nodiscard]] std::optional<EncodedMemory> encode_gather_memory(
	const Gadget &gadget, const Operand &operand,
	std::uint16_t destination_offset) noexcept
{
	if (operand.kind != OperandKind::memory ||
		!operand.memory.vector_index ||
		operand.memory.index == MemoryOperand::no_register)
		return std::nullopt;
	auto base = operand;
	base.memory.index = MemoryOperand::no_register;
	base.memory.vector_index = false;
	return encode_memory(gadget, base, destination_offset);
}

[[nodiscard]] bool append_memory_move(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool load = instruction.source.kind == OperandKind::memory;
	const auto &memory_operand = load ?
		instruction.source : instruction.destination;
	const auto &value_operand = load ?
		instruction.destination : instruction.source;
	MemoryMoveForm form{};
	std::uint16_t value_offset{};
	if (load) {
		const auto offset = register_offset(value_operand);
		if (!offset)
			return false;
		value_offset = *offset;
		form = MemoryMoveForm::load;
	} else if (value_operand.kind == OperandKind::immediate) {
		form = MemoryMoveForm::store_immediate;
	} else {
		const auto offset = register_offset(value_operand);
		if (!offset)
			return false;
		value_offset = *offset;
		form = MemoryMoveForm::store_register;
	}
	const auto encoded = encode_memory(gadget, memory_operand, value_offset);
	const auto entry = memory_move_entry(memory_operand.width, form);
	if (!encoded || entry == 0)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	if (form == MemoryMoveForm::store_immediate)
		stream.push_back(value_operand.immediate);
	return true;
}

[[nodiscard]] bool append_memory_arithmetic(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool register_memory = instruction.source.kind ==
		OperandKind::memory;
	const auto &memory_operand = register_memory ?
		instruction.source : instruction.destination;
	const auto &value_operand = register_memory ?
		instruction.destination : instruction.source;
	MemoryArithmeticForm form{};
	std::uint16_t value_offset{};
	if (register_memory) {
		const auto offset = register_offset(value_operand);
		if (!offset)
			return false;
		value_offset = *offset;
		form = MemoryArithmeticForm::register_memory;
	} else if (value_operand.kind == OperandKind::immediate) {
		form = MemoryArithmeticForm::memory_immediate;
	} else {
		const auto offset = register_offset(value_operand);
		if (!offset)
			return false;
		value_offset = *offset;
		form = MemoryArithmeticForm::memory_register;
	}
	const auto encoded = encode_memory(gadget, memory_operand, value_offset);
	const auto entry = memory_arithmetic_entry(
		instruction.operation, memory_operand.width, form,
		instruction.locked);
	if (!encoded || entry == 0)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	if (form == MemoryArithmeticForm::memory_immediate)
		stream.push_back(value_operand.immediate);
	return true;
}

[[nodiscard]] bool append_shift(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const bool immediate = instruction.source.kind == OperandKind::immediate;
	if ((!memory && instruction.destination.kind != OperandKind::reg) ||
		(!immediate && instruction.source.kind != OperandKind::reg))
		return false;
	const auto form = memory ?
		(immediate ? ShiftForm::memory_immediate :
			ShiftForm::memory_register) :
		(immediate ? ShiftForm::register_immediate :
			ShiftForm::register_register);
	const auto entry = shift_entry(instruction.operation,
		instruction.destination.width, form);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry);
		if (immediate) {
			stream.push_back(*destination);
			stream.push_back(instruction.source.immediate);
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}

	std::uint16_t count_offset{};
	if (!immediate) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		count_offset = *source;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, count_offset);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	if (immediate)
		stream.push_back(instruction.source.immediate);
	return true;
}

[[nodiscard]] bool append_double_shift(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const bool immediate = instruction.auxiliary.kind == OperandKind::immediate;
	if ((!memory && instruction.destination.kind != OperandKind::reg) ||
		instruction.source.kind != OperandKind::reg ||
		(!immediate && instruction.auxiliary.kind != OperandKind::reg))
		return false;
	const auto source = register_offset(instruction.source);
	if (!source)
		return false;
	const auto form = memory ?
		(immediate ? DoubleShiftForm::memory_immediate :
			DoubleShiftForm::memory_register) :
		(immediate ? DoubleShiftForm::register_immediate :
			DoubleShiftForm::register_register);
	const auto entry = double_shift_entry(instruction.operation,
		instruction.destination.width, form);
	if (entry == 0)
		return false;
	std::uintptr_t count{};
	if (immediate) {
		count = instruction.auxiliary.immediate;
	} else {
		const auto count_offset = register_offset(instruction.auxiliary);
		if (!count_offset)
			return false;
		count = *count_offset;
	}
	if (!memory) {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		stream.push_back(count);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, *source);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	stream.push_back(count);
	return true;
}

[[nodiscard]] bool append_multiply(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool sign = instruction.operation == Operation::multiply_signed;
	const bool implicit = instruction.source.kind == OperandKind::none;
	if (implicit) {
		const bool memory = instruction.destination.kind == OperandKind::memory;
		if (!memory && instruction.destination.kind != OperandKind::reg)
			return false;
		const auto form = memory ? MultiplyForm::implicit_memory :
			MultiplyForm::implicit_register;
		const auto entry = multiply_entry(
			sign, instruction.destination.width, form);
		if (entry == 0)
			return false;
		if (!memory) {
			const auto source = register_offset(instruction.destination);
			if (!source)
				return false;
			stream.push_back(entry);
			stream.push_back(*source);
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.destination, 0);
		if (!encoded)
			return false;
		append_memory_words(stream, entry, *encoded, gadget);
		return true;
	}

	if (!sign || instruction.destination.kind != OperandKind::reg)
		return false;
	const bool immediate = instruction.auxiliary.kind != OperandKind::none;
	if (immediate && instruction.auxiliary.kind != OperandKind::immediate)
		return false;
	const bool memory = instruction.source.kind == OperandKind::memory;
	if (!memory && instruction.source.kind != OperandKind::reg)
		return false;
	const auto destination = register_offset(instruction.destination);
	if (!destination)
		return false;
	const auto form = memory ?
		(immediate ? MultiplyForm::immediate_memory :
			MultiplyForm::explicit_memory) :
		(immediate ? MultiplyForm::immediate_register :
			MultiplyForm::explicit_register);
	const auto entry = multiply_entry(
		true, instruction.destination.width, form);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		if (immediate)
			stream.push_back(instruction.auxiliary.immediate);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.source, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	if (immediate)
		stream.push_back(instruction.auxiliary.immediate);
	return true;
}

[[nodiscard]] bool append_divide(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	if (!memory && instruction.destination.kind != OperandKind::reg)
		return false;
	const auto entry = divide_entry(
		instruction.operation == Operation::divide_signed,
		instruction.destination.width, memory);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto divisor = register_offset(instruction.destination);
		if (!divisor)
			return false;
		stream.push_back(entry);
		stream.push_back(*divisor | stream.locator(gadget));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, 0);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_bit_operation(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const bool immediate = instruction.source.kind == OperandKind::immediate;
	if ((!memory && instruction.destination.kind != OperandKind::reg) ||
		(!immediate && instruction.source.kind != OperandKind::reg))
		return false;
	const auto form = memory ?
		(immediate ? ShiftForm::memory_immediate :
			ShiftForm::memory_register) :
		(immediate ? ShiftForm::register_immediate :
			ShiftForm::register_register);
	const auto entry = bit_operation_entry(instruction.operation,
		instruction.destination.width, form, instruction.locked);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry);
		if (immediate) {
			stream.push_back(*destination);
			stream.push_back(instruction.source.immediate);
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	std::uint16_t index_offset{};
	if (!immediate) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		index_offset = *source;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, index_offset);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	if (immediate)
		stream.push_back(instruction.source.immediate);
	return true;
}

[[nodiscard]] bool append_bit_count(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind != OperandKind::reg)
		return false;
	const auto destination = register_offset(instruction.destination);
	const bool memory = instruction.source.kind == OperandKind::memory;
	if (!destination || (!memory && instruction.source.kind != OperandKind::reg))
		return false;
	const auto entry = bit_count_entry(
		instruction.operation, instruction.source.width, memory);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.source, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_atomic_binary(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	if ((!memory && instruction.destination.kind != OperandKind::reg) ||
		instruction.source.kind != OperandKind::reg)
		return false;
	const auto source = register_offset(instruction.source);
	const auto entry = atomic_binary_entry(instruction.operation,
		instruction.destination.width, memory);
	if (!source || entry == 0)
		return false;
	if (!memory) {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, *source);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_compare_exchange_pair(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind != OperandKind::memory)
		return false;
	const auto size = instruction.auxiliary.immediate;
	const auto encoded = encode_memory(
		gadget, instruction.destination, 0);
	if (!encoded || (size != 8 && size != 16))
		return false;
	append_memory_words(stream, entry_address(size == 16 ?
		native::uml_x86_64_gadget_cmpxchg_pair_16 :
		native::uml_x86_64_gadget_cmpxchg_pair_8), *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_movbe(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool load = instruction.source.kind == OperandKind::memory;
	const auto &memory_operand = load ?
		instruction.source : instruction.destination;
	const auto &register_operand_value = load ?
		instruction.destination : instruction.source;
	if (memory_operand.kind != OperandKind::memory ||
		register_operand_value.kind != OperandKind::reg)
		return false;
	const auto register_value = register_offset(register_operand_value);
	const auto entry = movbe_entry(memory_operand.width, load);
	const auto encoded = register_value ? encode_memory(
		gadget, memory_operand, *register_value) : std::nullopt;
	if (!encoded || entry == 0)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_crc32(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind != OperandKind::reg)
		return false;
	const auto destination = register_offset(instruction.destination);
	const bool memory = instruction.source.kind == OperandKind::memory;
	if (!destination || (!memory && instruction.source.kind != OperandKind::reg))
		return false;
	const auto entry = crc32_entry(instruction.source.width, memory);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.source, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_adx(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind != OperandKind::reg)
		return false;
	const auto destination = register_offset(instruction.destination);
	const bool memory = instruction.source.kind == OperandKind::memory;
	if (!destination || (!memory && instruction.source.kind != OperandKind::reg))
		return false;
	const auto entry = adx_entry(instruction.vector_control != 0,
		instruction.destination.width, memory);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.source, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_scalar_bmi(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind != OperandKind::reg)
		return false;
	const auto destination = register_offset(instruction.destination);
	if (!destination)
		return false;

	if (instruction.vector_opcode == 0xf3) {
		BmiKind kind{};
		switch (instruction.vector_group) {
		case 1: kind = BmiKind::blsr; break;
		case 2: kind = BmiKind::blsmsk; break;
		case 3: kind = BmiKind::blsi; break;
		default: return false;
		}
		const bool memory = instruction.source.kind == OperandKind::memory;
		if (!memory && instruction.source.kind != OperandKind::reg)
			return false;
		const auto entry = bmi_unary_entry(
			kind, instruction.destination.width, memory);
		if (entry == 0)
			return false;
		if (!memory) {
			const auto source = register_offset(instruction.source);
			if (!source)
				return false;
			stream.push_back(entry);
			stream.push_back(pack_offsets(*destination, *source));
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.source, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry, *encoded, gadget);
		return true;
	}

	if (instruction.vector_opcode == 0xf0) {
		const bool memory = instruction.source.kind == OperandKind::memory;
		if (!memory && instruction.source.kind != OperandKind::reg)
			return false;
		const auto entry = rorx_entry(instruction.destination.width, memory);
		if (entry == 0)
			return false;
		if (!memory) {
			const auto source = register_offset(instruction.source);
			if (!source)
				return false;
			stream.push_back(entry);
			stream.push_back(pack_offsets(*destination, *source));
			stream.push_back(instruction.vector_control);
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.source, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry, *encoded, gadget);
		stream.push_back(instruction.vector_control);
		return true;
	}

	BmiKind kind{};
	switch (instruction.vector_opcode) {
	case 0xf2: kind = BmiKind::andn; break;
	case 0xf5:
		kind = instruction.vector_prefix == 3 ? BmiKind::pdep :
			instruction.vector_prefix == 2 ? BmiKind::pext : BmiKind::bzhi;
		break;
	case 0xf6: kind = BmiKind::mulx; break;
	case 0xf7:
		switch (instruction.vector_prefix) {
		case 0: kind = BmiKind::bextr; break;
		case 1: kind = BmiKind::shlx; break;
		case 2: kind = BmiKind::sarx; break;
		case 3: kind = BmiKind::shrx; break;
		default: return false;
		}
		break;
	default: return false;
	}
	if (instruction.source.kind != OperandKind::reg)
		return false;
	const auto first = register_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	if (!first || (!memory && instruction.auxiliary.kind != OperandKind::reg))
		return false;
	const auto entry = bmi_ternary_entry(
		kind, instruction.destination.width, memory);
	if (entry == 0)
		return false;
	if (!memory) {
		const auto second = register_offset(instruction.auxiliary);
		if (!second)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *first, *second));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	stream.push_back(*first);
	return true;
}

[[nodiscard]] bool append_vector_move(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind == OperandKind::mmx_reg) {
		const auto destination = mmx_offset(instruction.destination);
		if (!destination)
			return false;
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(
				gadget, instruction.source, *destination);
			if (!encoded || instruction.source.width != OperandWidth::qword)
				return false;
			append_memory_words(stream, entry_address(
				native::uml_x86_64_gadget_vector_move_64_mmx_rm),
				*encoded, gadget);
			return true;
		}
		const auto source = vector_state_offset(instruction.source);
		if (!source || instruction.source.width != OperandWidth::qword)
			return false;
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_vector_move_64_mmx_rr));
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	if (instruction.destination.kind == OperandKind::vector_reg &&
		(instruction.source.kind == OperandKind::vector_reg ||
		 instruction.source.kind == OperandKind::mmx_reg)) {
		const auto destination = vector_offset(instruction.destination);
		const auto source = vector_state_offset(instruction.source);
		if (!destination || !source)
			return false;
		std::uintptr_t entry{};
		if (instruction.source.width == OperandWidth::qword &&
			instruction.destination.width == OperandWidth::xmm) {
			entry = entry_address(instruction.vex ?
				native::uml_x86_64_gadget_vector_move_64_128_rr_vex :
				native::uml_x86_64_gadget_vector_move_64_128_rr);
		} else if (instruction.source.kind == OperandKind::vector_reg &&
			instruction.source.width == instruction.destination.width) {
			entry = instruction.destination.width == OperandWidth::ymm ? entry_address(
				native::uml_x86_64_gadget_vector_move_256_rr) :
				instruction.destination.width == OperandWidth::xmm ?
				entry_address(instruction.vex ?
					native::uml_x86_64_gadget_vector_move_128_rr_vex :
					native::uml_x86_64_gadget_vector_move_128_rr) : 0;
		}
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	if (instruction.destination.kind == OperandKind::vector_reg &&
		instruction.source.kind == OperandKind::memory) {
		const auto destination = vector_offset(instruction.destination);
		const auto encoded = destination ? encode_memory(
			gadget, instruction.source, *destination) : std::nullopt;
		if (!encoded)
			return false;
		std::uintptr_t entry{};
		if (instruction.source.width == OperandWidth::qword &&
			instruction.destination.width == OperandWidth::xmm)
			entry = entry_address(
				native::uml_x86_64_gadget_vector_move_64_128_rm);
		else if (instruction.source.width == OperandWidth::ymm)
			entry = entry_address(native::uml_x86_64_gadget_vector_move_256_rm);
		else if (instruction.source.width == OperandWidth::xmm)
			entry = entry_address(instruction.vex ?
				native::uml_x86_64_gadget_vector_move_128_rm_vex :
				native::uml_x86_64_gadget_vector_move_128_rm);
		if (entry == 0)
			return false;
		append_memory_words(stream, entry, *encoded, gadget);
		return true;
	}
	if (instruction.destination.kind == OperandKind::memory &&
		(instruction.source.kind == OperandKind::vector_reg ||
		 instruction.source.kind == OperandKind::mmx_reg)) {
		const auto source = vector_state_offset(instruction.source);
		const auto encoded = source ? encode_memory(
			gadget, instruction.destination, *source) : std::nullopt;
		if (!encoded)
			return false;
		std::uintptr_t entry{};
		switch (instruction.destination.width) {
		case OperandWidth::dword:
			entry = entry_address(native::uml_x86_64_gadget_vector_move_32_mr);
			break;
		case OperandWidth::qword:
			entry = entry_address(native::uml_x86_64_gadget_vector_move_64_mr);
			break;
		case OperandWidth::xmm:
			entry = entry_address(native::uml_x86_64_gadget_vector_move_128_mr);
			break;
		case OperandWidth::ymm:
			entry = entry_address(native::uml_x86_64_gadget_vector_move_256_mr);
			break;
		default: return false;
		}
		append_memory_words(stream, entry, *encoded, gadget);
		return true;
	}
	return false;
}

[[nodiscard]] bool append_vector_move_scalar(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.destination.kind == OperandKind::memory &&
		instruction.source.kind == OperandKind::vector_reg) {
		const auto source = vector_offset(instruction.source);
		const auto encoded = source ? encode_memory(
			gadget, instruction.destination, *source) : std::nullopt;
		if (!encoded)
			return false;
		NativeEntry entry{};
		if (instruction.destination.width == OperandWidth::dword)
			entry = native::uml_x86_64_gadget_vector_move_32_mr;
		else if (instruction.destination.width == OperandWidth::qword)
			entry = native::uml_x86_64_gadget_vector_move_64_mr;
		else
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget);
		return true;
	}
	if (instruction.destination.kind != OperandKind::vector_reg)
		return false;
	const auto destination = vector_offset(instruction.destination);
	if (!destination)
		return false;
	const bool dword = instruction.source.width == OperandWidth::dword;
	if (!dword && instruction.source.width != OperandWidth::qword)
		return false;
	if (instruction.source.kind == OperandKind::memory) {
		const auto encoded = encode_memory(
			gadget, instruction.source, *destination);
		if (!encoded)
			return false;
		const auto entry = dword ?
			(instruction.vex ?
				native::uml_x86_64_gadget_vector_scalar_move_32_rm_vex :
				native::uml_x86_64_gadget_vector_scalar_move_32_rm) :
			(instruction.vex ?
				native::uml_x86_64_gadget_vector_scalar_move_64_rm_vex :
				native::uml_x86_64_gadget_vector_scalar_move_64_rm);
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget);
		return true;
	}
	const auto source = vector_offset(instruction.source);
	if (!source)
		return false;
	if (instruction.vex) {
		const auto merge = vector_offset(instruction.auxiliary);
		if (!merge)
			return false;
		stream.push_back(entry_address(dword ?
			native::uml_x86_64_gadget_vector_scalar_move_32_rr_vex :
			native::uml_x86_64_gadget_vector_scalar_move_64_rr_vex));
		stream.push_back(pack_offsets(*destination, *source, *merge));
		return true;
	}
	stream.push_back(entry_address(dword ?
		native::uml_x86_64_gadget_vector_scalar_move_32_rr :
		native::uml_x86_64_gadget_vector_scalar_move_64_rr));
	stream.push_back(pack_offsets(*destination, *source));
	return true;
}

[[nodiscard]] bool append_vector_to_gpr(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = register_offset(instruction.destination);
	const auto source = vector_state_offset(instruction.source);
	if (!destination || !source)
		return false;
	NativeEntry entry{};
	if (instruction.vector_opcode == 0x50) {
		const bool doubles = instruction.vector_prefix == 1;
		if (instruction.source.width == OperandWidth::xmm)
			entry = doubles ? native::uml_x86_64_gadget_vector_movmsk_64_128 :
				native::uml_x86_64_gadget_vector_movmsk_32_128;
		else if (instruction.source.width == OperandWidth::ymm)
			entry = doubles ? native::uml_x86_64_gadget_vector_movmsk_64_256 :
				native::uml_x86_64_gadget_vector_movmsk_32_256;
	} else if (instruction.vector_opcode == 0xd7) {
		switch (instruction.source.width) {
		case OperandWidth::qword:
			entry = native::uml_x86_64_gadget_vector_pmovmskb_64;
			break;
		case OperandWidth::xmm:
			entry = native::uml_x86_64_gadget_vector_pmovmskb_128;
			break;
		case OperandWidth::ymm:
			entry = native::uml_x86_64_gadget_vector_pmovmskb_256;
			break;
		default: break;
		}
	} else if (instruction.vector_opcode == 0xc5) {
		switch (instruction.source.width) {
		case OperandWidth::qword:
			entry = native::uml_x86_64_gadget_vector_pextrw_64;
			break;
		case OperandWidth::xmm:
			entry = native::uml_x86_64_gadget_vector_pextrw_128;
			break;
		case OperandWidth::ymm:
			entry = native::uml_x86_64_gadget_vector_pextrw_256;
			break;
		default: break;
		}
	}
	if (entry == nullptr)
		return false;
	stream.push_back(entry_address(entry));
	stream.push_back(pack_offsets(*destination, *source));
	if (instruction.vector_opcode == 0xc5)
		stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] bool append_vector_insert(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_state_offset(instruction.destination);
	const auto merge = vector_state_offset(instruction.source);
	const bool mmx = instruction.destination.kind == OperandKind::mmx_reg;
	if (!destination || !merge || (!mmx &&
		instruction.destination.kind != OperandKind::vector_reg))
		return false;
	NativeEntry register_entry{};
	NativeEntry memory_entry{};
	if (mmx) {
		register_entry = native::uml_x86_64_gadget_vector_insert_64_rr;
		memory_entry = native::uml_x86_64_gadget_vector_insert_64_rm;
	} else if (instruction.vex) {
		register_entry = native::uml_x86_64_gadget_vector_insert_128_rr_vex;
		memory_entry = native::uml_x86_64_gadget_vector_insert_128_rm_vex;
	} else {
		register_entry = native::uml_x86_64_gadget_vector_insert_128_rr;
		memory_entry = native::uml_x86_64_gadget_vector_insert_128_rm;
	}
	if (instruction.auxiliary.kind == OperandKind::reg) {
		const auto input = register_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(register_entry));
		stream.push_back(pack_offsets(*destination, *merge, *input));
		stream.push_back(instruction.vector_control);
		return true;
	}
	if (instruction.auxiliary.kind != OperandKind::memory)
		return false;
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(memory_entry), *encoded,
		gadget);
	stream.push_back(static_cast<std::uintptr_t>(*merge) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

[[nodiscard]] NativeEntry vector_test_entry(
	OperandWidth width, unsigned stride, bool memory) noexcept
{
#define UML_X86_VECTOR_TEST_ENTRY(width_bits, stride_value) \
	return memory ? native::uml_x86_64_gadget_vector_test_##width_bits##_##stride_value##_rm : \
		native::uml_x86_64_gadget_vector_test_##width_bits##_##stride_value##_rr
	if (width == OperandWidth::xmm) {
		switch (stride) {
		case 1: UML_X86_VECTOR_TEST_ENTRY(128, 1);
		case 4: UML_X86_VECTOR_TEST_ENTRY(128, 4);
		case 8: UML_X86_VECTOR_TEST_ENTRY(128, 8);
		default: return nullptr;
		}
	}
	if (width == OperandWidth::ymm) {
		switch (stride) {
		case 1: UML_X86_VECTOR_TEST_ENTRY(256, 1);
		case 4: UML_X86_VECTOR_TEST_ENTRY(256, 4);
		case 8: UML_X86_VECTOR_TEST_ENTRY(256, 8);
		default: return nullptr;
		}
	}
#undef UML_X86_VECTOR_TEST_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_test(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = vector_offset(instruction.source);
	const unsigned stride = instruction.vector_map == 2 &&
		instruction.vector_opcode == 0x0f ? 8U :
		instruction.vector_map == 2 && instruction.vector_opcode == 0x0e ?
			4U : 1U;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_test_entry(
		instruction.source.width, stride, memory);
	if (!left || entry == nullptr)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*left, *right));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *left);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_vector_compare_flags(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto left = vector_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const bool doubles = instruction.vector_prefix == 1;
	/* COMIS raises invalid operation for a quiet NaN too, UCOMIS only for
	 * a signalling one. */
	const auto entry = instruction.vector_opcode == 0x2f ?
		(doubles ?
			(memory ? native::uml_x86_64_gadget_vector_compare_signalling_64_rm :
				native::uml_x86_64_gadget_vector_compare_signalling_64_rr) :
			(memory ? native::uml_x86_64_gadget_vector_compare_signalling_32_rm :
				native::uml_x86_64_gadget_vector_compare_signalling_32_rr)) :
		(doubles ?
			(memory ? native::uml_x86_64_gadget_vector_compare_64_rm :
				native::uml_x86_64_gadget_vector_compare_64_rr) :
			(memory ? native::uml_x86_64_gadget_vector_compare_32_rm :
				native::uml_x86_64_gadget_vector_compare_32_rr));
	if (!left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*left, *right));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *left);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_vector_gpr_move(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode == 0x7e &&
		instruction.vector_prefix == 2) {
		const auto destination = vector_offset(instruction.destination);
		if (!destination)
			return false;
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(
				gadget, instruction.source, *destination);
			if (!encoded)
				return false;
			append_memory_words(stream, entry_address(
				instruction.vex ?
				native::uml_x86_64_gadget_vector_move_64_128_rm_vex :
				native::uml_x86_64_gadget_vector_move_64_128_rm),
				*encoded, gadget);
			return true;
		}
		const auto source = vector_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry_address(instruction.vex ?
			native::uml_x86_64_gadget_vector_move_64_128_rr_vex :
			native::uml_x86_64_gadget_vector_move_64_128_rr));
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	if (instruction.vector_opcode == 0x6e) {
		const bool mmx = instruction.destination.kind == OperandKind::mmx_reg;
		const auto destination = vector_state_offset(instruction.destination);
		const bool dword = instruction.source.width == OperandWidth::dword;
		if (!destination || (!dword &&
			instruction.source.width != OperandWidth::qword))
			return false;
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(
				gadget, instruction.source, *destination);
			if (!encoded)
				return false;
			NativeEntry entry{};
			if (mmx)
				entry = dword ?
					native::uml_x86_64_gadget_vector_memory_32_to_mmx :
					native::uml_x86_64_gadget_vector_move_64_mmx_rm;
			else if (dword)
				entry = instruction.vex ?
					native::uml_x86_64_gadget_vector_scalar_move_32_rm_vex :
					native::uml_x86_64_gadget_vector_scalar_move_32_rm;
			else
				entry = instruction.vex ?
					native::uml_x86_64_gadget_vector_scalar_move_64_rm_vex :
					native::uml_x86_64_gadget_vector_scalar_move_64_rm;
			append_memory_words(stream, entry_address(entry),
				*encoded, gadget);
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		NativeEntry entry{};
		if (mmx)
			entry = dword ? native::uml_x86_64_gadget_vector_gpr_to_mmx_32 :
				native::uml_x86_64_gadget_vector_gpr_to_mmx_64;
		else if (dword)
			entry = instruction.vex ?
				native::uml_x86_64_gadget_vector_gpr_to_xmm_32_vex :
				native::uml_x86_64_gadget_vector_gpr_to_xmm_32;
		else
			entry = instruction.vex ?
				native::uml_x86_64_gadget_vector_gpr_to_xmm_64_vex :
				native::uml_x86_64_gadget_vector_gpr_to_xmm_64;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *source));
		return true;
	}
	if (instruction.vector_opcode != 0x7e)
		return false;
	const auto source = vector_state_offset(instruction.source);
	const bool dword = instruction.destination.width == OperandWidth::dword;
	if (!source || (!dword &&
		instruction.destination.width != OperandWidth::qword))
		return false;
	if (instruction.destination.kind == OperandKind::memory) {
		const auto encoded = encode_memory(
			gadget, instruction.destination, *source);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(dword ?
			native::uml_x86_64_gadget_vector_move_32_mr :
			native::uml_x86_64_gadget_vector_move_64_mr),
			*encoded, gadget);
		return true;
	}
	const auto destination = register_offset(instruction.destination);
	if (!destination)
		return false;
	stream.push_back(select_move(instruction.destination.width, false));
	stream.push_back(pack_offsets(*destination, *source));
	return true;
}

[[nodiscard]] bool append_vector_mask_store(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto source = vector_state_offset(instruction.source);
	const auto mask = vector_state_offset(instruction.auxiliary);
	if (!source || !mask || instruction.source.width !=
		instruction.auxiliary.width)
		return false;
	NativeEntry entry{};
	if (instruction.source.width == OperandWidth::qword)
		entry = native::uml_x86_64_gadget_vector_mask_store_64;
	else if (instruction.source.width == OperandWidth::xmm)
		entry = native::uml_x86_64_gadget_vector_mask_store_128;
	else
		return false;
	const auto encoded = encode_memory(
		gadget, instruction.destination, *source);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*mask);
	return true;
}

enum class VectorDuplicateKind { sldup, shdup, ddup };

[[nodiscard]] NativeEntry vector_duplicate_entry(VectorDuplicateKind kind,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_DUPLICATE_ENTRY(kind_name, width_bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_duplicate_##kind_name##_##width_bits##_rm : \
		native::uml_x86_64_gadget_vector_duplicate_##kind_name##_##width_bits##_rr
	if (width == OperandWidth::xmm) {
		switch (kind) {
		case VectorDuplicateKind::sldup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(sldup, 128);
		case VectorDuplicateKind::shdup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(shdup, 128);
		case VectorDuplicateKind::ddup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(ddup, 128);
		}
	}
	if (width == OperandWidth::ymm) {
		switch (kind) {
		case VectorDuplicateKind::sldup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(sldup, 256);
		case VectorDuplicateKind::shdup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(shdup, 256);
		case VectorDuplicateKind::ddup:
			UML_X86_VECTOR_DUPLICATE_ENTRY(ddup, 256);
		}
	}
#undef UML_X86_VECTOR_DUPLICATE_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_lane_move(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode == 0x13 ||
		instruction.vector_opcode == 0x17) {
		const auto source = vector_offset(instruction.source);
		const auto value_offset = source ? static_cast<std::uint16_t>(
			*source + (instruction.vector_opcode == 0x17 ? 8U : 0U)) : 0;
		const auto encoded = source ? encode_memory(
			gadget, instruction.destination, value_offset) : std::nullopt;
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(
			native::uml_x86_64_gadget_vector_move_64_mr),
			*encoded, gadget);
		return true;
	}

	const bool duplicate = (instruction.vector_opcode == 0x12 &&
		(instruction.vector_prefix == 2 ||
		 instruction.vector_prefix == 3)) ||
		(instruction.vector_opcode == 0x16 &&
		 instruction.vector_prefix == 2);
	const auto destination = vector_offset(instruction.destination);
	if (!destination)
		return false;
	if (duplicate) {
		const auto kind = instruction.vector_prefix == 3 ?
			VectorDuplicateKind::ddup : instruction.vector_opcode == 0x16 ?
				VectorDuplicateKind::shdup : VectorDuplicateKind::sldup;
		const bool memory = instruction.auxiliary.kind == OperandKind::memory;
		const auto entry = vector_duplicate_entry(
			kind, instruction.destination.width, memory);
		if (entry == nullptr)
			return false;
		if (!memory) {
			const auto input = vector_offset(instruction.auxiliary);
			if (!input)
				return false;
			stream.push_back(entry_address(entry));
			stream.push_back(pack_offsets(*destination, *input) |
				vex_tag(instruction));
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.auxiliary, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget, DescriptorTag::vex);
		return true;
	}

	const auto merge = vector_offset(instruction.source);
	if (!merge)
		return false;
	const bool destination_high = instruction.vector_opcode == 0x16;
	if (instruction.auxiliary.kind == OperandKind::vector_reg) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		const bool source_high = instruction.vector_opcode == 0x12;
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_vector_lane_merge_rr));
		stream.push_back(static_cast<std::uintptr_t>(*destination) |
			(static_cast<std::uintptr_t>(*merge) << 16U) |
			(static_cast<std::uintptr_t>(*input) << 32U) |
			(static_cast<std::uintptr_t>(destination_high) << 48U) |
			(static_cast<std::uintptr_t>(source_high) << 49U) |
			(static_cast<std::uintptr_t>(instruction.vex) << 50U));
		return true;
	}
	if (instruction.auxiliary.kind != OperandKind::memory)
		return false;
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(
		native::uml_x86_64_gadget_vector_lane_merge_rm),
		*encoded, gadget);
	stream.push_back(static_cast<std::uintptr_t>(*merge) |
		(static_cast<std::uintptr_t>(destination_high) << 16U) |
		(static_cast<std::uintptr_t>(instruction.vex) << 17U));
	return true;
}

[[nodiscard]] std::uintptr_t vector_bitwise_entry(std::uint8_t opcode,
	OperandWidth width, bool vex, bool memory) noexcept
{
#define UML_X86_VECTOR_BITWISE_ENTRY(operation) \
	if (width == OperandWidth::ymm) \
		return entry_address(memory ? \
			native::uml_x86_64_gadget_vector_##operation##_256_rm : \
			native::uml_x86_64_gadget_vector_##operation##_256_rr); \
	if (width == OperandWidth::xmm) { \
		if (memory) \
			return entry_address(vex ? \
				native::uml_x86_64_gadget_vector_##operation##_128_rm_vex : \
				native::uml_x86_64_gadget_vector_##operation##_128_rm); \
		return entry_address(vex ? \
			native::uml_x86_64_gadget_vector_##operation##_128_rr_vex : \
			native::uml_x86_64_gadget_vector_##operation##_128_rr); \
	} \
	return 0
	switch (opcode) {
	case 0x54: UML_X86_VECTOR_BITWISE_ENTRY(and);
	case 0x55: UML_X86_VECTOR_BITWISE_ENTRY(andnot);
	case 0x56: UML_X86_VECTOR_BITWISE_ENTRY(or);
	case 0x57: UML_X86_VECTOR_BITWISE_ENTRY(xor);
	default: return 0;
	}
#undef UML_X86_VECTOR_BITWISE_ENTRY
}

[[nodiscard]] bool append_vector_bitwise(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode < 0x54 ||
		instruction.vector_opcode > 0x57 ||
		instruction.destination.kind != OperandKind::vector_reg ||
		instruction.source.kind != OperandKind::vector_reg)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (instruction.auxiliary.kind == OperandKind::vector_reg) {
		const auto right = vector_offset(instruction.auxiliary);
		const auto entry = vector_bitwise_entry(instruction.vector_opcode,
			instruction.destination.width, instruction.vex, false);
		if (!right || entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(pack_offsets(*destination, *left, *right));
		return true;
	}
	if (instruction.auxiliary.kind == OperandKind::memory) {
		const auto encoded = encode_memory(
			gadget, instruction.auxiliary, *destination);
		const auto entry = vector_bitwise_entry(instruction.vector_opcode,
			instruction.destination.width, instruction.vex, true);
		if (!encoded || entry == 0)
			return false;
		append_memory_words(stream, entry, *encoded, gadget);
		stream.push_back(*left);
		return true;
	}
	return false;
}

enum class VectorShuffleKind {
	unpack_low,
	unpack_high,
	shuffle,
	hadd,
	hsub,
	addsub,
};

[[nodiscard]] NativeEntry vector_shuffle_entry(VectorShuffleKind kind,
	unsigned element, OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_SHUFFLE_ENTRY(operation, bits, width_bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_##operation##_##bits##_##width_bits##_rm : \
		native::uml_x86_64_gadget_vector_##operation##_##bits##_##width_bits##_rr
#define UML_X86_VECTOR_SHUFFLE_KIND(operation) \
	case VectorShuffleKind::operation: \
		if (element == 32 && width == OperandWidth::xmm) \
			UML_X86_VECTOR_SHUFFLE_ENTRY(operation, 32, 128); \
		if (element == 32 && width == OperandWidth::ymm) \
			UML_X86_VECTOR_SHUFFLE_ENTRY(operation, 32, 256); \
		if (element == 64 && width == OperandWidth::xmm) \
			UML_X86_VECTOR_SHUFFLE_ENTRY(operation, 64, 128); \
		if (element == 64 && width == OperandWidth::ymm) \
			UML_X86_VECTOR_SHUFFLE_ENTRY(operation, 64, 256); \
		break
	switch (kind) {
	UML_X86_VECTOR_SHUFFLE_KIND(unpack_low);
	UML_X86_VECTOR_SHUFFLE_KIND(unpack_high);
	UML_X86_VECTOR_SHUFFLE_KIND(shuffle);
	UML_X86_VECTOR_SHUFFLE_KIND(hadd);
	UML_X86_VECTOR_SHUFFLE_KIND(hsub);
	UML_X86_VECTOR_SHUFFLE_KIND(addsub);
	}
#undef UML_X86_VECTOR_SHUFFLE_KIND
#undef UML_X86_VECTOR_SHUFFLE_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_shuffle(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	VectorShuffleKind kind{};
	switch (instruction.vector_opcode) {
	case 0x14: kind = VectorShuffleKind::unpack_low; break;
	case 0x15: kind = VectorShuffleKind::unpack_high; break;
	case 0xc6: kind = VectorShuffleKind::shuffle; break;
	case 0x7c: kind = VectorShuffleKind::hadd; break;
	case 0x7d: kind = VectorShuffleKind::hsub; break;
	case 0xd0: kind = VectorShuffleKind::addsub; break;
	default: return false;
	}
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const unsigned element = instruction.vector_prefix == 1 ? 64U : 32U;
	const auto entry = vector_shuffle_entry(
		kind, element, instruction.destination.width, memory);
	if (!destination || !left || entry == nullptr)
		return false;
	const bool control = instruction.vector_opcode == 0xc6;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		if (control)
			stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(static_cast<std::uintptr_t>(*left) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

enum class VectorFloatKind {
	square_root,
	reciprocal_square_root,
	reciprocal,
	add,
	multiply,
	subtract,
	minimum,
	divide,
	maximum,
};

[[nodiscard]] NativeEntry vector_float_entry(VectorFloatKind kind,
	unsigned element, OperandWidth width, bool scalar, bool memory) noexcept
{
#define UML_X86_VECTOR_FP_ENTRY(operation, bits, shape) \
	return memory ? \
		native::uml_x86_64_gadget_vector_fp_##operation##_##bits##_##shape##_rm : \
		native::uml_x86_64_gadget_vector_fp_##operation##_##bits##_##shape##_rr
#define UML_X86_VECTOR_FP_SHAPE(operation) \
	if (element == 32 && scalar) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, scalar); \
	if (element == 64 && scalar) \
		UML_X86_VECTOR_FP_ENTRY(operation, 64, scalar); \
	if (element == 32 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, 128); \
	if (element == 64 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 64, 128); \
	if (element == 32 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, 256); \
	if (element == 64 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 64, 256); \
	break
	/* The decoder accepts the estimates' single-precision forms only. */
#define UML_X86_VECTOR_FP_SINGLE_SHAPE(operation) \
	if (element == 32 && scalar) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, scalar); \
	if (element == 32 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, 128); \
	if (element == 32 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_FP_ENTRY(operation, 32, 256); \
	break
	switch (kind) {
	case VectorFloatKind::square_root:
		UML_X86_VECTOR_FP_SHAPE(sqrt);
	case VectorFloatKind::reciprocal_square_root:
		UML_X86_VECTOR_FP_SINGLE_SHAPE(rsqrt);
	case VectorFloatKind::reciprocal:
		UML_X86_VECTOR_FP_SINGLE_SHAPE(rcp);
	case VectorFloatKind::add:
		UML_X86_VECTOR_FP_SHAPE(add);
	case VectorFloatKind::multiply:
		UML_X86_VECTOR_FP_SHAPE(mul);
	case VectorFloatKind::subtract:
		UML_X86_VECTOR_FP_SHAPE(sub);
	case VectorFloatKind::minimum:
		UML_X86_VECTOR_FP_SHAPE(min);
	case VectorFloatKind::divide:
		UML_X86_VECTOR_FP_SHAPE(div);
	case VectorFloatKind::maximum:
		UML_X86_VECTOR_FP_SHAPE(max);
	}
#undef UML_X86_VECTOR_FP_SINGLE_SHAPE
#undef UML_X86_VECTOR_FP_SHAPE
#undef UML_X86_VECTOR_FP_ENTRY
	return nullptr;
}

[[nodiscard]] NativeEntry vector_float_compare_entry(unsigned predicate,
	unsigned element, OperandWidth width, bool scalar, bool memory) noexcept
{
#define UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, bits, shape) \
	return memory ? \
		native::uml_x86_64_gadget_vector_fp_cmp_##predicate_value##_##bits##_##shape##_rm : \
		native::uml_x86_64_gadget_vector_fp_cmp_##predicate_value##_##bits##_##shape##_rr
#define UML_X86_VECTOR_FP_COMPARE_SHAPE(predicate_value) \
	if (element == 32 && scalar) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 32, scalar); \
	if (element == 64 && scalar) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 64, scalar); \
	if (element == 32 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 32, 128); \
	if (element == 64 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 64, 128); \
	if (element == 32 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 32, 256); \
	if (element == 64 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_FP_COMPARE_ENTRY(predicate_value, 64, 256); \
	break
	switch (predicate & 0x1fU) {
	case 0: UML_X86_VECTOR_FP_COMPARE_SHAPE(0);
	case 1: UML_X86_VECTOR_FP_COMPARE_SHAPE(1);
	case 2: UML_X86_VECTOR_FP_COMPARE_SHAPE(2);
	case 3: UML_X86_VECTOR_FP_COMPARE_SHAPE(3);
	case 4: UML_X86_VECTOR_FP_COMPARE_SHAPE(4);
	case 5: UML_X86_VECTOR_FP_COMPARE_SHAPE(5);
	case 6: UML_X86_VECTOR_FP_COMPARE_SHAPE(6);
	case 7: UML_X86_VECTOR_FP_COMPARE_SHAPE(7);
	case 8: UML_X86_VECTOR_FP_COMPARE_SHAPE(8);
	case 9: UML_X86_VECTOR_FP_COMPARE_SHAPE(9);
	case 10: UML_X86_VECTOR_FP_COMPARE_SHAPE(10);
	case 11: UML_X86_VECTOR_FP_COMPARE_SHAPE(11);
	case 12: UML_X86_VECTOR_FP_COMPARE_SHAPE(12);
	case 13: UML_X86_VECTOR_FP_COMPARE_SHAPE(13);
	case 14: UML_X86_VECTOR_FP_COMPARE_SHAPE(14);
	case 15: UML_X86_VECTOR_FP_COMPARE_SHAPE(15);
	case 16: UML_X86_VECTOR_FP_COMPARE_SHAPE(16);
	case 17: UML_X86_VECTOR_FP_COMPARE_SHAPE(17);
	case 18: UML_X86_VECTOR_FP_COMPARE_SHAPE(18);
	case 19: UML_X86_VECTOR_FP_COMPARE_SHAPE(19);
	case 20: UML_X86_VECTOR_FP_COMPARE_SHAPE(20);
	case 21: UML_X86_VECTOR_FP_COMPARE_SHAPE(21);
	case 22: UML_X86_VECTOR_FP_COMPARE_SHAPE(22);
	case 23: UML_X86_VECTOR_FP_COMPARE_SHAPE(23);
	case 24: UML_X86_VECTOR_FP_COMPARE_SHAPE(24);
	case 25: UML_X86_VECTOR_FP_COMPARE_SHAPE(25);
	case 26: UML_X86_VECTOR_FP_COMPARE_SHAPE(26);
	case 27: UML_X86_VECTOR_FP_COMPARE_SHAPE(27);
	case 28: UML_X86_VECTOR_FP_COMPARE_SHAPE(28);
	case 29: UML_X86_VECTOR_FP_COMPARE_SHAPE(29);
	case 30: UML_X86_VECTOR_FP_COMPARE_SHAPE(30);
	case 31: UML_X86_VECTOR_FP_COMPARE_SHAPE(31);
	default: break;
	}
#undef UML_X86_VECTOR_FP_COMPARE_SHAPE
#undef UML_X86_VECTOR_FP_COMPARE_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_float(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool scalar = instruction.vector_prefix >= 2;
	const unsigned element = instruction.vector_prefix == 1 ||
		instruction.vector_prefix == 3 ? 64U : 32U;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	NativeEntry entry{};
	if (instruction.vector_opcode == 0xc2) {
		/* The legacy encoding reads the immediate's low three bits, as x86
		 * hardware does, and VEX its low five; bit 4 changes only which
		 * NaNs raise invalid operation, and its predicates have gadgets of
		 * their own. */
		entry = vector_float_compare_entry(instruction.vex ?
				instruction.vector_control : instruction.vector_control & 7U,
			element, instruction.destination.width, scalar, memory);
	} else {
		VectorFloatKind kind{};
		switch (instruction.vector_opcode) {
		case 0x51: kind = VectorFloatKind::square_root; break;
		case 0x52: kind = VectorFloatKind::reciprocal_square_root; break;
		case 0x53: kind = VectorFloatKind::reciprocal; break;
		case 0x58: kind = VectorFloatKind::add; break;
		case 0x59: kind = VectorFloatKind::multiply; break;
		case 0x5c: kind = VectorFloatKind::subtract; break;
		case 0x5d: kind = VectorFloatKind::minimum; break;
		case 0x5e: kind = VectorFloatKind::divide; break;
		case 0x5f: kind = VectorFloatKind::maximum; break;
		default: return false;
		}
		entry = vector_float_entry(
			kind, element, instruction.destination.width, scalar, memory);
	}
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	if (entry == nullptr || !destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(pack_offsets(*left) | vex_tag(instruction));
	return true;
}

[[nodiscard]] NativeEntry vector_convert_entry(const Instruction &instruction,
	bool memory) noexcept
{
#define UML_X86_VECTOR_CONVERT_ENTRY(name) \
	return memory ? native::uml_x86_64_gadget_vector_convert_##name##_rm : \
		native::uml_x86_64_gadget_vector_convert_##name##_rr
	if (instruction.vector_opcode == 0x5a) {
		switch (instruction.vector_prefix) {
		case 0:
			if (instruction.destination.width == OperandWidth::ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(ps_pd_256);
			UML_X86_VECTOR_CONVERT_ENTRY(ps_pd_128);
		case 1:
			if (instruction.auxiliary.width == OperandWidth::ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(pd_ps_256);
			UML_X86_VECTOR_CONVERT_ENTRY(pd_ps_128);
		case 2: UML_X86_VECTOR_CONVERT_ENTRY(ss_sd);
		case 3: UML_X86_VECTOR_CONVERT_ENTRY(sd_ss);
		default: break;
		}
	}
	if (instruction.vector_opcode == 0x5b) {
		const bool ymm = instruction.destination.width == OperandWidth::ymm;
		if (instruction.vector_prefix == 0) {
			if (ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(dq_ps_256);
			UML_X86_VECTOR_CONVERT_ENTRY(dq_ps_128);
		}
		if (instruction.vector_prefix == 1) {
			if (ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(ps_dq_256);
			UML_X86_VECTOR_CONVERT_ENTRY(ps_dq_128);
		}
		if (instruction.vector_prefix == 2) {
			if (ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(ttps_dq_256);
			UML_X86_VECTOR_CONVERT_ENTRY(ttps_dq_128);
		}
	}
	if (instruction.vector_opcode == 0xe6) {
		if (instruction.vector_prefix == 2) {
			if (instruction.destination.width == OperandWidth::ymm)
				UML_X86_VECTOR_CONVERT_ENTRY(dq_pd_256);
			UML_X86_VECTOR_CONVERT_ENTRY(dq_pd_128);
		}
		const bool wide = instruction.auxiliary.width == OperandWidth::ymm;
		if (instruction.vector_prefix == 1) {
			if (wide)
				UML_X86_VECTOR_CONVERT_ENTRY(ttpd_dq_256);
			UML_X86_VECTOR_CONVERT_ENTRY(ttpd_dq_128);
		}
		if (instruction.vector_prefix == 3) {
			if (wide)
				UML_X86_VECTOR_CONVERT_ENTRY(pd_dq_256);
			UML_X86_VECTOR_CONVERT_ENTRY(pd_dq_128);
		}
	}
#undef UML_X86_VECTOR_CONVERT_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_convert(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const bool scalar = instruction.vector_opcode == 0x5a &&
		instruction.vector_prefix >= 2;
	const auto merge = scalar ? vector_offset(instruction.source) : destination;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_convert_entry(instruction, memory);
	if (!destination || !merge || entry == nullptr)
		return false;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *merge, *input) |
			vex_tag(instruction));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(pack_offsets(*merge) | vex_tag(instruction));
	return true;
}

[[nodiscard]] NativeEntry integer_to_scalar_entry(OperandWidth integer_width,
	unsigned element, bool memory) noexcept
{
#define UML_X86_INTEGER_TO_SCALAR_ENTRY(integer, fp) \
	return memory ? native::uml_x86_64_gadget_vector_scalar_i##integer##_f##fp##_rm : \
		native::uml_x86_64_gadget_vector_scalar_i##integer##_f##fp##_rr
	if (integer_width == OperandWidth::dword && element == 32)
		UML_X86_INTEGER_TO_SCALAR_ENTRY(32, 32);
	if (integer_width == OperandWidth::qword && element == 32)
		UML_X86_INTEGER_TO_SCALAR_ENTRY(64, 32);
	if (integer_width == OperandWidth::dword && element == 64)
		UML_X86_INTEGER_TO_SCALAR_ENTRY(32, 64);
	if (integer_width == OperandWidth::qword && element == 64)
		UML_X86_INTEGER_TO_SCALAR_ENTRY(64, 64);
#undef UML_X86_INTEGER_TO_SCALAR_ENTRY
	return nullptr;
}

[[nodiscard]] NativeEntry scalar_to_integer_entry(unsigned element,
	OperandWidth integer_width, bool truncate, bool memory) noexcept
{
#define UML_X86_SCALAR_TO_INTEGER_ENTRY(fp, integer, truncation) \
	return memory ? \
		native::uml_x86_64_gadget_vector_scalar_f##fp##_i##integer##_##truncation##_rm : \
		native::uml_x86_64_gadget_vector_scalar_f##fp##_i##integer##_##truncation##_rr
	if (element == 32 && integer_width == OperandWidth::dword) {
		if (truncate)
			UML_X86_SCALAR_TO_INTEGER_ENTRY(32, 32, 1);
		UML_X86_SCALAR_TO_INTEGER_ENTRY(32, 32, 0);
	}
	if (element == 32 && integer_width == OperandWidth::qword) {
		if (truncate)
			UML_X86_SCALAR_TO_INTEGER_ENTRY(32, 64, 1);
		UML_X86_SCALAR_TO_INTEGER_ENTRY(32, 64, 0);
	}
	if (element == 64 && integer_width == OperandWidth::dword) {
		if (truncate)
			UML_X86_SCALAR_TO_INTEGER_ENTRY(64, 32, 1);
		UML_X86_SCALAR_TO_INTEGER_ENTRY(64, 32, 0);
	}
	if (element == 64 && integer_width == OperandWidth::qword) {
		if (truncate)
			UML_X86_SCALAR_TO_INTEGER_ENTRY(64, 64, 1);
		UML_X86_SCALAR_TO_INTEGER_ENTRY(64, 64, 0);
	}
#undef UML_X86_SCALAR_TO_INTEGER_ENTRY
	return nullptr;
}

[[nodiscard]] NativeEntry packed_integer_to_float_entry(
	unsigned element, bool memory) noexcept
{
	if (element == 32)
		return memory ? native::uml_x86_64_gadget_vector_pi_f32_rm :
			native::uml_x86_64_gadget_vector_pi_f32_rr;
	if (element == 64)
		return memory ? native::uml_x86_64_gadget_vector_pi_f64_rm :
			native::uml_x86_64_gadget_vector_pi_f64_rr;
	return nullptr;
}

[[nodiscard]] NativeEntry packed_float_to_integer_entry(unsigned element,
	bool truncate, bool memory) noexcept
{
#define UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY(fp, truncation) \
	return memory ? native::uml_x86_64_gadget_vector_f##fp##_pi_##truncation##_rm : \
		native::uml_x86_64_gadget_vector_f##fp##_pi_##truncation##_rr
	if (element == 32) {
		if (truncate)
			UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY(32, 1);
		UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY(32, 0);
	}
	if (element == 64) {
		if (truncate)
			UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY(64, 1);
		UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY(64, 0);
	}
#undef UML_X86_PACKED_FLOAT_TO_INTEGER_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_scalar_convert(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool to_float = instruction.vector_opcode == 0x2a;
	const bool scalar = instruction.vector_prefix >= 2;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const unsigned element = instruction.vector_prefix == 1 ||
		instruction.vector_prefix == 3 ? 64U : 32U;
	if (to_float) {
		const auto destination = vector_offset(instruction.destination);
		const auto merge = vector_offset(instruction.source);
		if (!destination || !merge)
			return false;
		const auto entry = scalar ? integer_to_scalar_entry(
			instruction.auxiliary.width, element, memory) :
			packed_integer_to_float_entry(element, memory);
		if (entry == nullptr)
			return false;
		if (!memory) {
			const auto input = scalar ? register_offset(instruction.auxiliary) :
				mmx_offset(instruction.auxiliary);
			if (!input)
				return false;
			stream.push_back(entry_address(entry));
			stream.push_back(pack_offsets(*destination, *merge,
				*input) | vex_tag(instruction));
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.auxiliary, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget, DescriptorTag::vex);
		stream.push_back(pack_offsets(*merge) | vex_tag(instruction));
		return true;
	}

	const bool truncate = instruction.vector_opcode == 0x2c;
	if (scalar) {
		const auto destination = register_offset(instruction.destination);
		const auto entry = scalar_to_integer_entry(element,
			instruction.destination.width, truncate, memory);
		if (!destination || entry == nullptr)
			return false;
		if (!memory) {
			const auto input = vector_offset(instruction.auxiliary);
			if (!input)
				return false;
			stream.push_back(entry_address(entry));
			stream.push_back(pack_offsets(*destination, *input));
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.auxiliary, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget);
		return true;
	}

	const auto destination = mmx_offset(instruction.destination);
	const auto entry = packed_float_to_integer_entry(
		element, truncate, memory);
	if (!destination || entry == nullptr)
		return false;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *input));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] NativeEntry vector_fma_entry(std::uint8_t opcode,
	unsigned element, OperandWidth width, bool scalar, bool memory) noexcept
{
#define UML_X86_FMA_ENTRY(symbol, bits, shape) \
	return memory ? native::uml_x86_64_gadget_vector_fma_##symbol##_##bits##_##shape##_rm : \
		native::uml_x86_64_gadget_vector_fma_##symbol##_##bits##_##shape##_rr
#define UML_X86_FMA_SHAPE(symbol) \
	if (element == 32 && scalar) \
		UML_X86_FMA_ENTRY(symbol, 32, scalar); \
	if (element == 64 && scalar) \
		UML_X86_FMA_ENTRY(symbol, 64, scalar); \
	if (element == 32 && width == OperandWidth::xmm) \
		UML_X86_FMA_ENTRY(symbol, 32, 128); \
	if (element == 64 && width == OperandWidth::xmm) \
		UML_X86_FMA_ENTRY(symbol, 64, 128); \
	if (element == 32 && width == OperandWidth::ymm) \
		UML_X86_FMA_ENTRY(symbol, 32, 256); \
	if (element == 64 && width == OperandWidth::ymm) \
		UML_X86_FMA_ENTRY(symbol, 64, 256); \
	break
#define UML_X86_FMA_CASE(value, symbol) \
	case value: UML_X86_FMA_SHAPE(symbol)
	switch (opcode) {
	UML_X86_FMA_CASE(0x96, 96);
	UML_X86_FMA_CASE(0x97, 97);
	UML_X86_FMA_CASE(0x98, 98);
	UML_X86_FMA_CASE(0x99, 99);
	UML_X86_FMA_CASE(0x9a, 9a);
	UML_X86_FMA_CASE(0x9b, 9b);
	UML_X86_FMA_CASE(0x9c, 9c);
	UML_X86_FMA_CASE(0x9d, 9d);
	UML_X86_FMA_CASE(0x9e, 9e);
	UML_X86_FMA_CASE(0x9f, 9f);
	UML_X86_FMA_CASE(0xa6, a6);
	UML_X86_FMA_CASE(0xa7, a7);
	UML_X86_FMA_CASE(0xa8, a8);
	UML_X86_FMA_CASE(0xa9, a9);
	UML_X86_FMA_CASE(0xaa, aa);
	UML_X86_FMA_CASE(0xab, ab);
	UML_X86_FMA_CASE(0xac, ac);
	UML_X86_FMA_CASE(0xad, ad);
	UML_X86_FMA_CASE(0xae, ae);
	UML_X86_FMA_CASE(0xaf, af);
	UML_X86_FMA_CASE(0xb6, b6);
	UML_X86_FMA_CASE(0xb7, b7);
	UML_X86_FMA_CASE(0xb8, b8);
	UML_X86_FMA_CASE(0xb9, b9);
	UML_X86_FMA_CASE(0xba, ba);
	UML_X86_FMA_CASE(0xbb, bb);
	UML_X86_FMA_CASE(0xbc, bc);
	UML_X86_FMA_CASE(0xbd, bd);
	UML_X86_FMA_CASE(0xbe, be);
	UML_X86_FMA_CASE(0xbf, bf);
	default: break;
	}
#undef UML_X86_FMA_CASE
#undef UML_X86_FMA_SHAPE
#undef UML_X86_FMA_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_fma(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const auto vvvv = vector_offset(instruction.source);
	const auto operation = static_cast<std::uint8_t>(
		instruction.vector_opcode & 0xfU);
	const bool scalar = operation >= 9U && (operation & 1U) != 0;
	const unsigned element = instruction.vector_group != 0 ? 64U : 32U;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_fma_entry(instruction.vector_opcode,
		element, instruction.destination.width, scalar, memory);
	if (!destination || !vvvv || entry == nullptr)
		return false;
	if (!memory) {
		const auto rm = vector_offset(instruction.auxiliary);
		if (!rm)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *vvvv, *rm));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*vvvv);
	return true;
}

[[nodiscard]] NativeEntry vector_dot_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_DOT_ENTRY(symbol, bits) \
	return memory ? native::uml_x86_64_gadget_vector_dot_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_dot_##symbol##_##bits##_rr
#define UML_X86_DOT_CASE(value, symbol) \
	case value: \
		if (width == OperandWidth::xmm) \
			UML_X86_DOT_ENTRY(symbol, 128); \
		if (width == OperandWidth::ymm) \
			UML_X86_DOT_ENTRY(symbol, 256); \
		break
	switch (opcode) {
	UML_X86_DOT_CASE(0x50, 50);
	UML_X86_DOT_CASE(0x51, 51);
	UML_X86_DOT_CASE(0x52, 52);
	UML_X86_DOT_CASE(0x53, 53);
	default: break;
	}
#undef UML_X86_DOT_CASE
#undef UML_X86_DOT_ENTRY
	return nullptr;
}

/* VPDPBUSD and its relatives accumulate into the destination, which is also
 * the first stream operand, like the FMA forms: destination, vvvv, r/m. */
[[nodiscard]] bool append_vector_dot(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const auto vvvv = vector_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_dot_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (!destination || !vvvv || entry == nullptr)
		return false;
	if (!memory) {
		const auto rm = vector_offset(instruction.auxiliary);
		if (!rm)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *vvvv, *rm));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*vvvv);
	return true;
}

[[nodiscard]] NativeEntry vector_aes_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_AES_ENTRY(symbol, bits) \
	return memory ? native::uml_x86_64_gadget_vector_aes_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_aes_##symbol##_##bits##_rr
	if (opcode == 0xdb && width == OperandWidth::xmm)
		UML_X86_AES_ENTRY(db, 128);
	if (opcode == 0xdc && width == OperandWidth::xmm)
		UML_X86_AES_ENTRY(dc, 128);
	if (opcode == 0xdc && width == OperandWidth::ymm)
		UML_X86_AES_ENTRY(dc, 256);
	if (opcode == 0xdd && width == OperandWidth::xmm)
		UML_X86_AES_ENTRY(dd, 128);
	if (opcode == 0xdd && width == OperandWidth::ymm)
		UML_X86_AES_ENTRY(dd, 256);
	if (opcode == 0xde && width == OperandWidth::xmm)
		UML_X86_AES_ENTRY(de, 128);
	if (opcode == 0xde && width == OperandWidth::ymm)
		UML_X86_AES_ENTRY(de, 256);
	if (opcode == 0xdf && width == OperandWidth::xmm)
		UML_X86_AES_ENTRY(df, 128);
	if (opcode == 0xdf && width == OperandWidth::ymm)
		UML_X86_AES_ENTRY(df, 256);
#undef UML_X86_AES_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_aes(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const auto state = instruction.source.kind == OperandKind::none ?
		destination : vector_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_aes_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (!destination || !state || entry == nullptr)
		return false;
	if (!memory) {
		const auto key = vector_offset(instruction.auxiliary);
		if (!key)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *state, *key) |
			vex_tag(instruction));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(pack_offsets(*state) | vex_tag(instruction));
	return true;
}

[[nodiscard]] NativeEntry vector_sha_entry(
	std::uint8_t opcode, bool memory) noexcept
{
#define UML_X86_SHA_ENTRY(value, symbol) \
	case value: return memory ? \
		native::uml_x86_64_gadget_vector_sha_##symbol##_rm : \
		native::uml_x86_64_gadget_vector_sha_##symbol##_rr
	switch (opcode) {
	UML_X86_SHA_ENTRY(0xc8, c8);
	UML_X86_SHA_ENTRY(0xc9, c9);
	UML_X86_SHA_ENTRY(0xca, ca);
	UML_X86_SHA_ENTRY(0xcb, cb);
	UML_X86_SHA_ENTRY(0xcc, cc);
	UML_X86_SHA_ENTRY(0xcd, cd);
	default: break;
	}
#undef UML_X86_SHA_ENTRY
	return nullptr;
}

[[nodiscard]] bool append_vector_sha(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const auto state = vector_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_sha_entry(instruction.vector_opcode, memory);
	if (!destination || !state || entry == nullptr)
		return false;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *state, *input));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*state);
	return true;
}

[[nodiscard]] NativeEntry vector_integer_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_INTEGER_WIDTH(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_integer_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_integer_##symbol##_##bits##_rr
#define UML_X86_VECTOR_INTEGER_SHAPE(symbol) \
	if (width == OperandWidth::qword) \
		UML_X86_VECTOR_INTEGER_WIDTH(symbol, 64); \
	if (width == OperandWidth::xmm) \
		UML_X86_VECTOR_INTEGER_WIDTH(symbol, 128); \
	if (width == OperandWidth::ymm) \
		UML_X86_VECTOR_INTEGER_WIDTH(symbol, 256); \
	break
#define UML_X86_VECTOR_INTEGER_CASE(value, symbol) \
	case value: UML_X86_VECTOR_INTEGER_SHAPE(symbol)
	switch (opcode) {
	UML_X86_VECTOR_INTEGER_CASE(0x60, 60);
	UML_X86_VECTOR_INTEGER_CASE(0x61, 61);
	UML_X86_VECTOR_INTEGER_CASE(0x62, 62);
	UML_X86_VECTOR_INTEGER_CASE(0x63, 63);
	UML_X86_VECTOR_INTEGER_CASE(0x64, 64);
	UML_X86_VECTOR_INTEGER_CASE(0x65, 65);
	UML_X86_VECTOR_INTEGER_CASE(0x66, 66);
	UML_X86_VECTOR_INTEGER_CASE(0x67, 67);
	UML_X86_VECTOR_INTEGER_CASE(0x68, 68);
	UML_X86_VECTOR_INTEGER_CASE(0x69, 69);
	UML_X86_VECTOR_INTEGER_CASE(0x6a, 6a);
	UML_X86_VECTOR_INTEGER_CASE(0x6b, 6b);
	UML_X86_VECTOR_INTEGER_CASE(0x6c, 6c);
	UML_X86_VECTOR_INTEGER_CASE(0x6d, 6d);
	UML_X86_VECTOR_INTEGER_CASE(0x70, 70);
	UML_X86_VECTOR_INTEGER_CASE(0x74, 74);
	UML_X86_VECTOR_INTEGER_CASE(0x75, 75);
	UML_X86_VECTOR_INTEGER_CASE(0x76, 76);
	UML_X86_VECTOR_INTEGER_CASE(0xd1, d1);
	UML_X86_VECTOR_INTEGER_CASE(0xd2, d2);
	UML_X86_VECTOR_INTEGER_CASE(0xd3, d3);
	UML_X86_VECTOR_INTEGER_CASE(0xd4, d4);
	UML_X86_VECTOR_INTEGER_CASE(0xd5, d5);
	UML_X86_VECTOR_INTEGER_CASE(0xd8, d8);
	UML_X86_VECTOR_INTEGER_CASE(0xd9, d9);
	UML_X86_VECTOR_INTEGER_CASE(0xda, da);
	UML_X86_VECTOR_INTEGER_CASE(0xdb, db);
	UML_X86_VECTOR_INTEGER_CASE(0xdc, dc);
	UML_X86_VECTOR_INTEGER_CASE(0xdd, dd);
	UML_X86_VECTOR_INTEGER_CASE(0xde, de);
	UML_X86_VECTOR_INTEGER_CASE(0xdf, df);
	UML_X86_VECTOR_INTEGER_CASE(0xe0, e0);
	UML_X86_VECTOR_INTEGER_CASE(0xe1, e1);
	UML_X86_VECTOR_INTEGER_CASE(0xe2, e2);
	UML_X86_VECTOR_INTEGER_CASE(0xe3, e3);
	UML_X86_VECTOR_INTEGER_CASE(0xe4, e4);
	UML_X86_VECTOR_INTEGER_CASE(0xe5, e5);
	UML_X86_VECTOR_INTEGER_CASE(0xe8, e8);
	UML_X86_VECTOR_INTEGER_CASE(0xe9, e9);
	UML_X86_VECTOR_INTEGER_CASE(0xea, ea);
	UML_X86_VECTOR_INTEGER_CASE(0xeb, eb);
	UML_X86_VECTOR_INTEGER_CASE(0xec, ec);
	UML_X86_VECTOR_INTEGER_CASE(0xed, ed);
	UML_X86_VECTOR_INTEGER_CASE(0xee, ee);
	UML_X86_VECTOR_INTEGER_CASE(0xef, ef);
	UML_X86_VECTOR_INTEGER_CASE(0xf1, f1);
	UML_X86_VECTOR_INTEGER_CASE(0xf2, f2);
	UML_X86_VECTOR_INTEGER_CASE(0xf3, f3);
	UML_X86_VECTOR_INTEGER_CASE(0xf4, f4);
	UML_X86_VECTOR_INTEGER_CASE(0xf5, f5);
	UML_X86_VECTOR_INTEGER_CASE(0xf6, f6);
	UML_X86_VECTOR_INTEGER_CASE(0xf8, f8);
	UML_X86_VECTOR_INTEGER_CASE(0xf9, f9);
	UML_X86_VECTOR_INTEGER_CASE(0xfa, fa);
	UML_X86_VECTOR_INTEGER_CASE(0xfb, fb);
	UML_X86_VECTOR_INTEGER_CASE(0xfc, fc);
	UML_X86_VECTOR_INTEGER_CASE(0xfd, fd);
	UML_X86_VECTOR_INTEGER_CASE(0xfe, fe);
	default: break;
	}
#undef UML_X86_VECTOR_INTEGER_CASE
#undef UML_X86_VECTOR_INTEGER_SHAPE
#undef UML_X86_VECTOR_INTEGER_WIDTH
	return nullptr;
}

[[nodiscard]] NativeEntry vector_integer_immediate_entry(
	std::uint8_t opcode, std::uint8_t group, OperandWidth width) noexcept
{
#define UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(symbol, group_value, bits) \
	return native::uml_x86_64_gadget_vector_integer_##symbol##_g##group_value##_##bits##_ri
#define UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(symbol, group_value) \
	if (width == OperandWidth::qword) \
		UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(symbol, group_value, 64); \
	if (width == OperandWidth::xmm) \
		UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(symbol, group_value, 128); \
	if (width == OperandWidth::ymm) \
		UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(symbol, group_value, 256); \
	return nullptr
	if (opcode == 0x71) {
		switch (group) {
		case 2: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(71, 2);
		case 4: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(71, 4);
		case 6: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(71, 6);
		default: return nullptr;
		}
	}
	if (opcode == 0x72) {
		switch (group) {
		case 2: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(72, 2);
		case 4: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(72, 4);
		case 6: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(72, 6);
		default: return nullptr;
		}
	}
	if (opcode == 0x73) {
		if ((group == 3 || group == 7) && width == OperandWidth::qword)
			return nullptr;
		switch (group) {
		case 2: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(73, 2);
		case 3:
			if (width == OperandWidth::xmm)
				UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 3, 128);
			if (width == OperandWidth::ymm)
				UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 3, 256);
			return nullptr;
		case 6: UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE(73, 6);
		case 7:
			if (width == OperandWidth::xmm)
				UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 7, 128);
			if (width == OperandWidth::ymm)
				UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 7, 256);
			return nullptr;
		default: return nullptr;
		}
	}
#undef UML_X86_VECTOR_INTEGER_IMMEDIATE_SHAPE
#undef UML_X86_VECTOR_INTEGER_IMMEDIATE_WIDTH
	return nullptr;
}

/* What the gadget before this one left in the vector result registers, for a
 * consumer that would otherwise read the state slot back. Zero means nothing
 * may be taken from them: every gadget that is not a whitelisted producer
 * leaves it so, because v4 and v5 are call-clobbered and a gadget that calls
 * out -- a memory form, a publication, a materializer -- destroys them. */
struct VectorResult final {
	std::uint16_t destination{};
	OperandWidth width{};
};

/* The register forms whose result stays in v4, and v5 with it at 256 bits,
 * after the gadget has written it to the state. A memory form calls out on
 * its miss and page-crossing paths, so only the register forms qualify, and
 * only at the two widths whose result fills those registers. */
[[nodiscard]] bool vector_integer_forwards(
	const Instruction &instruction) noexcept
{
	return instruction.auxiliary.kind != OperandKind::memory &&
		(instruction.destination.width == OperandWidth::xmm ||
		 instruction.destination.width == OperandWidth::ymm) &&
		!(instruction.vector_opcode >= 0x71 &&
		  instruction.vector_opcode <= 0x73);
}

/* What this gadget leaves in v4 and v5 for the gadget after it. */
inline void note_vector_result(const Instruction &instruction,
	VectorResult &produced, bool forwards) noexcept
{
	if (!forwards)
		return;
	if (const auto destination = vector_state_offset(instruction.destination))
		produced = {*destination, instruction.destination.width};
}

[[nodiscard]] bool append_vector_integer(StreamWords &stream,
	const Gadget &gadget, const VectorResult &available) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_state_offset(instruction.destination);
	const auto left = vector_state_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (instruction.vector_opcode >= 0x71 &&
		instruction.vector_opcode <= 0x73) {
		const auto entry = vector_integer_immediate_entry(
			instruction.vector_opcode, instruction.vector_group,
			instruction.destination.width);
		if (entry == nullptr)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}

	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_integer_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto shuffle = static_cast<std::uintptr_t>(
		instruction.vector_control) |
		(static_cast<std::uintptr_t>(instruction.vector_prefix) << 8U);
	if (!memory) {
		const auto right = vector_state_offset(instruction.auxiliary);
		if (!right)
			return false;
		const bool forwarded = vector_integer_forwards(instruction) &&
			available.destination == *left &&
			available.width == instruction.destination.width;
		stream.push_back(entry_address(entry));
		stream.push_back(static_cast<std::uintptr_t>(*destination) |
			(static_cast<std::uintptr_t>(*left) << 16U) |
			(static_cast<std::uintptr_t>(*right) << 32U) |
			(static_cast<std::uintptr_t>(forwarded ? 1U : 0U) << 62U) |
			vex_tag(instruction));
		if (instruction.vector_opcode == 0x70)
			stream.push_back(shuffle);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(pack_offsets(*left) | vex_tag(instruction));
	if (instruction.vector_opcode == 0x70)
		stream.push_back(shuffle);
	return true;
}

[[nodiscard]] NativeEntry vector_integer_map2_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP2_WIDTH(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_integer_map2_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_integer_map2_##symbol##_##bits##_rr
#define UML_X86_VECTOR_MAP2_SHAPE(symbol) \
	if (width == OperandWidth::qword) \
		UML_X86_VECTOR_MAP2_WIDTH(symbol, 64); \
	if (width == OperandWidth::xmm) \
		UML_X86_VECTOR_MAP2_WIDTH(symbol, 128); \
	if (width == OperandWidth::ymm) \
		UML_X86_VECTOR_MAP2_WIDTH(symbol, 256); \
	break
#define UML_X86_VECTOR_MAP2_CASE(value, symbol) \
	case value: UML_X86_VECTOR_MAP2_SHAPE(symbol)
	switch (opcode) {
	UML_X86_VECTOR_MAP2_CASE(0x00, 00);
	UML_X86_VECTOR_MAP2_CASE(0x01, 01);
	UML_X86_VECTOR_MAP2_CASE(0x02, 02);
	UML_X86_VECTOR_MAP2_CASE(0x03, 03);
	UML_X86_VECTOR_MAP2_CASE(0x04, 04);
	UML_X86_VECTOR_MAP2_CASE(0x05, 05);
	UML_X86_VECTOR_MAP2_CASE(0x06, 06);
	UML_X86_VECTOR_MAP2_CASE(0x07, 07);
	UML_X86_VECTOR_MAP2_CASE(0x08, 08);
	UML_X86_VECTOR_MAP2_CASE(0x09, 09);
	UML_X86_VECTOR_MAP2_CASE(0x0a, 0a);
	UML_X86_VECTOR_MAP2_CASE(0x0b, 0b);
	UML_X86_VECTOR_MAP2_CASE(0x10, 10);
	UML_X86_VECTOR_MAP2_CASE(0x14, 14);
	UML_X86_VECTOR_MAP2_CASE(0x15, 15);
	UML_X86_VECTOR_MAP2_CASE(0x1c, 1c);
	UML_X86_VECTOR_MAP2_CASE(0x1d, 1d);
	UML_X86_VECTOR_MAP2_CASE(0x1e, 1e);
	UML_X86_VECTOR_MAP2_CASE(0x20, 20);
	UML_X86_VECTOR_MAP2_CASE(0x21, 21);
	UML_X86_VECTOR_MAP2_CASE(0x22, 22);
	UML_X86_VECTOR_MAP2_CASE(0x23, 23);
	UML_X86_VECTOR_MAP2_CASE(0x24, 24);
	UML_X86_VECTOR_MAP2_CASE(0x25, 25);
	UML_X86_VECTOR_MAP2_CASE(0x28, 28);
	UML_X86_VECTOR_MAP2_CASE(0x29, 29);
	UML_X86_VECTOR_MAP2_CASE(0x2b, 2b);
	UML_X86_VECTOR_MAP2_CASE(0x30, 30);
	UML_X86_VECTOR_MAP2_CASE(0x31, 31);
	UML_X86_VECTOR_MAP2_CASE(0x32, 32);
	UML_X86_VECTOR_MAP2_CASE(0x33, 33);
	UML_X86_VECTOR_MAP2_CASE(0x34, 34);
	UML_X86_VECTOR_MAP2_CASE(0x35, 35);
	UML_X86_VECTOR_MAP2_CASE(0x37, 37);
	UML_X86_VECTOR_MAP2_CASE(0x38, 38);
	UML_X86_VECTOR_MAP2_CASE(0x39, 39);
	UML_X86_VECTOR_MAP2_CASE(0x3a, 3a);
	UML_X86_VECTOR_MAP2_CASE(0x3b, 3b);
	UML_X86_VECTOR_MAP2_CASE(0x3c, 3c);
	UML_X86_VECTOR_MAP2_CASE(0x3d, 3d);
	UML_X86_VECTOR_MAP2_CASE(0x3e, 3e);
	UML_X86_VECTOR_MAP2_CASE(0x3f, 3f);
	UML_X86_VECTOR_MAP2_CASE(0x40, 40);
	UML_X86_VECTOR_MAP2_CASE(0x41, 41);
	default: break;
	}
#undef UML_X86_VECTOR_MAP2_CASE
#undef UML_X86_VECTOR_MAP2_SHAPE
#undef UML_X86_VECTOR_MAP2_WIDTH
	return nullptr;
}

[[nodiscard]] bool append_vector_integer_map2(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_state_offset(instruction.destination);
	const auto left = instruction.source.kind == OperandKind::none ?
		destination : vector_state_offset(instruction.source);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_integer_map2_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (!destination || !left || entry == nullptr)
		return false;
	if (!memory) {
		const auto right = vector_state_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(pack_offsets(*left) | vex_tag(instruction));
	return true;
}

[[nodiscard]] NativeEntry vector_map2_permute_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP2_PERMUTE(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map2_permute_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map2_permute_##symbol##_##bits##_rr
#define UML_X86_VECTOR_MAP2_PERMUTE_CASE(value, symbol) \
	case value: \
		if (width == OperandWidth::xmm) \
			UML_X86_VECTOR_MAP2_PERMUTE(symbol, 128); \
		if (width == OperandWidth::ymm) \
			UML_X86_VECTOR_MAP2_PERMUTE(symbol, 256); \
		break
	switch (opcode) {
	UML_X86_VECTOR_MAP2_PERMUTE_CASE(0x0c, 0c);
	UML_X86_VECTOR_MAP2_PERMUTE_CASE(0x0d, 0d);
	case 0x16:
		if (width == OperandWidth::ymm)
			UML_X86_VECTOR_MAP2_PERMUTE(16, 256);
		break;
	case 0x36:
		if (width == OperandWidth::ymm)
			UML_X86_VECTOR_MAP2_PERMUTE(36, 256);
		break;
	default: break;
	}
#undef UML_X86_VECTOR_MAP2_PERMUTE_CASE
#undef UML_X86_VECTOR_MAP2_PERMUTE
	return nullptr;
}

[[nodiscard]] bool append_vector_map2_permute(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map2_permute_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*left);
	return true;
}

[[nodiscard]] NativeEntry vector_map2_shift_entry(std::uint8_t opcode,
	unsigned element, OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP2_SHIFT(symbol, lane, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map2_shift_##symbol##_##lane##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map2_shift_##symbol##_##lane##_##bits##_rr
#define UML_X86_VECTOR_MAP2_SHIFT_WIDTH(symbol) \
	if (element == 32 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_MAP2_SHIFT(symbol, 32, 128); \
	if (element == 32 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_MAP2_SHIFT(symbol, 32, 256); \
	if (element == 64 && width == OperandWidth::xmm) \
		UML_X86_VECTOR_MAP2_SHIFT(symbol, 64, 128); \
	if (element == 64 && width == OperandWidth::ymm) \
		UML_X86_VECTOR_MAP2_SHIFT(symbol, 64, 256); \
	break
	switch (opcode) {
	case 0x45: UML_X86_VECTOR_MAP2_SHIFT_WIDTH(45);
	case 0x46: UML_X86_VECTOR_MAP2_SHIFT_WIDTH(46);
	case 0x47: UML_X86_VECTOR_MAP2_SHIFT_WIDTH(47);
	default: break;
	}
#undef UML_X86_VECTOR_MAP2_SHIFT_WIDTH
#undef UML_X86_VECTOR_MAP2_SHIFT
	return nullptr;
}

[[nodiscard]] bool append_vector_map2_shift(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map2_shift_entry(instruction.vector_opcode,
		instruction.vector_group != 0 ? 64U : 32U,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*left);
	return true;
}

[[nodiscard]] NativeEntry vector_map2_broadcast_entry(unsigned element,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP2_BROADCAST(lane, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map2_broadcast_##lane##_##bits##_m : \
		native::uml_x86_64_gadget_vector_map2_broadcast_##lane##_##bits##_r
	if (element == 8 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_BROADCAST(8, 128);
	if (element == 8 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_BROADCAST(8, 256);
	if (element == 16 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_BROADCAST(16, 128);
	if (element == 16 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_BROADCAST(16, 256);
	if (element == 32 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_BROADCAST(32, 128);
	if (element == 32 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_BROADCAST(32, 256);
	if (element == 64 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_BROADCAST(64, 128);
	if (element == 64 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_BROADCAST(64, 256);
	if (element == 128 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_BROADCAST(128, 256);
#undef UML_X86_VECTOR_MAP2_BROADCAST
	return nullptr;
}

[[nodiscard]] bool append_vector_map2_broadcast(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	unsigned element{};
	switch (instruction.vector_opcode) {
	case 0x78: element = 8; break;
	case 0x79: element = 16; break;
	case 0x18:
	case 0x58: element = 32; break;
	case 0x19:
	case 0x59: element = 64; break;
	case 0x1a:
	case 0x5a: element = 128; break;
	default: return false;
	}
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map2_broadcast_entry(
		element, instruction.destination.width, memory);
	const auto destination = vector_offset(instruction.destination);
	if (entry == nullptr || !destination)
		return false;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *input));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_vector_map2_half(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode != 0x13)
		return false;
	const bool wide = instruction.destination.width == OperandWidth::ymm;
	if (!wide && instruction.destination.width != OperandWidth::xmm)
		return false;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = memory ?
		(wide ? native::uml_x86_64_gadget_vector_map2_half_256_m :
			native::uml_x86_64_gadget_vector_map2_half_128_m) :
		(wide ? native::uml_x86_64_gadget_vector_map2_half_256_r :
			native::uml_x86_64_gadget_vector_map2_half_128_r);
	const auto destination = vector_offset(instruction.destination);
	if (!destination)
		return false;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *input));
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] NativeEntry vector_map2_mask_entry(bool store,
	unsigned element, OperandWidth width) noexcept
{
#define UML_X86_VECTOR_MAP2_MASK(lane, bits) \
	return store ? \
		native::uml_x86_64_gadget_vector_map2_mask_store_##lane##_##bits : \
		native::uml_x86_64_gadget_vector_map2_mask_load_##lane##_##bits
	if (element == 32 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_MASK(32, 128);
	if (element == 32 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_MASK(32, 256);
	if (element == 64 && width == OperandWidth::xmm)
		UML_X86_VECTOR_MAP2_MASK(64, 128);
	if (element == 64 && width == OperandWidth::ymm)
		UML_X86_VECTOR_MAP2_MASK(64, 256);
#undef UML_X86_VECTOR_MAP2_MASK
	return nullptr;
}

[[nodiscard]] bool append_vector_map2_masked_memory(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const bool load = opcode == 0x2c || opcode == 0x2d || opcode == 0x8c;
	const bool store = opcode == 0x2e || opcode == 0x2f || opcode == 0x8e;
	if (!load && !store)
		return false;
	const auto entry = vector_map2_mask_entry(store,
		instruction.vector_group == 8 ? 64U : 32U,
		instruction.destination.width);
	const auto mask = vector_offset(instruction.source);
	if (entry == nullptr || !mask)
		return false;
	if (load) {
		const auto destination = vector_offset(instruction.destination);
		const auto encoded = destination ? encode_memory(
			gadget, instruction.auxiliary, *destination) : std::nullopt;
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget);
		stream.push_back(*mask);
		return true;
	}
	const auto data = vector_offset(instruction.auxiliary);
	const auto encoded = data ? encode_memory(
		gadget, instruction.destination, *data) : std::nullopt;
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(*mask);
	return true;
}

[[nodiscard]] bool append_vector_map2_misc(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	if (append_vector_map2_masked_memory(stream, gadget))
		return true;
	if (append_vector_map2_permute(stream, gadget))
		return true;
	if (append_vector_map2_shift(stream, gadget))
		return true;
	if (append_vector_map2_broadcast(stream, gadget))
		return true;
	return append_vector_map2_half(stream, gadget);
}

[[nodiscard]] NativeEntry vector_gather_entry(std::uint8_t group) noexcept
{
	switch (group & 7U) {
	case 0: return native::uml_x86_64_gadget_vector_gather_d4_i4_r16;
	case 1: return native::uml_x86_64_gadget_vector_gather_d8_i4_r16;
	case 2: return native::uml_x86_64_gadget_vector_gather_d4_i8_r16;
	case 3: return native::uml_x86_64_gadget_vector_gather_d8_i8_r16;
	case 4: return native::uml_x86_64_gadget_vector_gather_d4_i4_r32;
	case 5: return native::uml_x86_64_gadget_vector_gather_d8_i4_r32;
	case 6: return native::uml_x86_64_gadget_vector_gather_d4_i8_r32;
	case 7: return native::uml_x86_64_gadget_vector_gather_d8_i8_r32;
	default: return nullptr;
	}
}

[[nodiscard]] bool append_vector_gather(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = vector_offset(instruction.destination);
	const auto mask = vector_offset(instruction.source);
	const auto &addressing = instruction.auxiliary;
	if (!destination || !mask || addressing.kind != OperandKind::memory ||
		!addressing.memory.vector_index ||
		addressing.memory.index >= 16U)
		return false;
	const auto encoded = encode_gather_memory(
		gadget, addressing, *destination);
	if (!encoded)
		return false;
	const auto index = static_cast<std::uint16_t>(
		offsetof(CpuState, vectors) + addressing.memory.index *
		sizeof(CpuState::vectors[0]));
	append_memory_words(stream, entry_address(vector_gather_entry(
		instruction.vector_group)), *encoded, gadget);
	stream.push_back(pack_offsets(*mask, index));
	return true;
}

[[nodiscard]] NativeEntry vector_map3_extract_entry(
	unsigned size, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP3_EXTRACT(bits) \
	return memory ? native::uml_x86_64_gadget_vector_map3_extract_##bits##_m : \
		native::uml_x86_64_gadget_vector_map3_extract_##bits##_r
	switch (size) {
	case 1: UML_X86_VECTOR_MAP3_EXTRACT(1);
	case 2: UML_X86_VECTOR_MAP3_EXTRACT(2);
	case 4: UML_X86_VECTOR_MAP3_EXTRACT(4);
	case 8: UML_X86_VECTOR_MAP3_EXTRACT(8);
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_EXTRACT
	return nullptr;
}

[[nodiscard]] bool append_vector_map3_extract_scalar(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode < 0x14 ||
		instruction.vector_opcode > 0x17)
		return false;
	unsigned size = instruction.vector_opcode == 0x14 ? 1U :
		(instruction.vector_opcode == 0x15 ? 2U : 4U);
	if (instruction.vector_opcode == 0x16 &&
		instruction.destination.width == OperandWidth::qword)
		size = 8;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const auto entry = vector_map3_extract_entry(size, memory);
	const auto source = vector_offset(instruction.source);
	if (entry == nullptr || !source)
		return false;
	if (!memory) {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *source));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(gadget, instruction.destination, 0);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(static_cast<std::uintptr_t>(*source) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

[[nodiscard]] bool append_vector_map3_extract_lane(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode != 0x19 &&
		instruction.vector_opcode != 0x39)
		return false;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const auto source = vector_offset(instruction.source);
	if (!source)
		return false;
	if (!memory) {
		const auto destination = vector_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_vector_map3_extract_128_r));
		stream.push_back(pack_offsets(*destination, *source));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(gadget, instruction.destination, 0);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(
		native::uml_x86_64_gadget_vector_map3_extract_128_m),
		*encoded, gadget);
	stream.push_back(static_cast<std::uintptr_t>(*source) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

[[nodiscard]] NativeEntry vector_map3_insert_entry(
	unsigned size, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP3_INSERT(bits) \
	return memory ? native::uml_x86_64_gadget_vector_map3_insert_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map3_insert_##bits##_rr
	switch (size) {
	case 1: UML_X86_VECTOR_MAP3_INSERT(1);
	case 4: UML_X86_VECTOR_MAP3_INSERT(4);
	case 8: UML_X86_VECTOR_MAP3_INSERT(8);
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_INSERT
	return nullptr;
}

[[nodiscard]] bool append_vector_map3_insert_scalar(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode != 0x20 &&
		instruction.vector_opcode != 0x22)
		return false;
	const unsigned size = instruction.vector_opcode == 0x20 ? 1U :
		(instruction.auxiliary.width == OperandWidth::qword ? 8U : 4U);
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map3_insert_entry(size, memory);
	const auto destination = vector_offset(instruction.destination);
	const auto merge = vector_offset(instruction.source);
	if (entry == nullptr || !destination || !merge)
		return false;
	if (!memory) {
		const auto input = register_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *merge, *input) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(static_cast<std::uintptr_t>(*merge) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

[[nodiscard]] bool append_vector_map3_insertps(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode != 0x21)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto merge = vector_offset(instruction.source);
	if (!destination || !merge)
		return false;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	if (!memory) {
		const auto input = vector_offset(instruction.auxiliary);
		if (!input)
			return false;
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_vector_map3_insertps_rr));
		stream.push_back(pack_offsets(*destination, *merge, *input) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(
		native::uml_x86_64_gadget_vector_map3_insertps_rm),
		*encoded, gadget, DescriptorTag::vex);
	stream.push_back(static_cast<std::uintptr_t>(*merge) |
		(static_cast<std::uintptr_t>(instruction.vector_control) << 16U));
	return true;
}

[[nodiscard]] NativeEntry vector_map3_shuffle_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP3_SHUFFLE_WIDTH(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map3_shuffle_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map3_shuffle_##symbol##_##bits##_rr
#define UML_X86_VECTOR_MAP3_SHUFFLE_SHAPE(symbol) \
	if (width == OperandWidth::qword) \
		UML_X86_VECTOR_MAP3_SHUFFLE_WIDTH(symbol, 64); \
	if (width == OperandWidth::xmm) \
		UML_X86_VECTOR_MAP3_SHUFFLE_WIDTH(symbol, 128); \
	if (width == OperandWidth::ymm) \
		UML_X86_VECTOR_MAP3_SHUFFLE_WIDTH(symbol, 256); \
	break
#define UML_X86_VECTOR_MAP3_SHUFFLE_CASE(value, symbol) \
	case value: UML_X86_VECTOR_MAP3_SHUFFLE_SHAPE(symbol)
	switch (opcode) {
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x00, 00);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x01, 01);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x02, 02);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x04, 04);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x05, 05);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x06, 06);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x0c, 0c);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x0d, 0d);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x0e, 0e);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x0f, 0f);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x18, 18);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x38, 38);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x46, 46);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x4a, 4a);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x4b, 4b);
	UML_X86_VECTOR_MAP3_SHUFFLE_CASE(0x4c, 4c);
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_SHUFFLE_CASE
#undef UML_X86_VECTOR_MAP3_SHUFFLE_SHAPE
#undef UML_X86_VECTOR_MAP3_SHUFFLE_WIDTH
	return nullptr;
}

[[nodiscard]] bool append_vector_map3_shuffle(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map3_shuffle_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto destination = vector_state_offset(instruction.destination);
	const auto left = instruction.source.kind == OperandKind::none ?
		destination : vector_state_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_state_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(*left);
	stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] NativeEntry vector_map3_round_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP3_ROUND(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map3_round_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map3_round_##symbol##_##bits##_rr
	switch (opcode) {
	case 0x08:
		if (width == OperandWidth::xmm)
			UML_X86_VECTOR_MAP3_ROUND(08, 128);
		if (width == OperandWidth::ymm)
			UML_X86_VECTOR_MAP3_ROUND(08, 256);
		break;
	case 0x09:
		if (width == OperandWidth::xmm)
			UML_X86_VECTOR_MAP3_ROUND(09, 128);
		if (width == OperandWidth::ymm)
			UML_X86_VECTOR_MAP3_ROUND(09, 256);
		break;
	case 0x0a:
		if (width == OperandWidth::xmm)
			UML_X86_VECTOR_MAP3_ROUND(0a, 128);
		break;
	case 0x0b:
		if (width == OperandWidth::xmm)
			UML_X86_VECTOR_MAP3_ROUND(0b, 128);
		break;
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_ROUND
	return nullptr;
}

[[nodiscard]] bool append_vector_map3_round(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map3_round_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto left = instruction.source.kind == OperandKind::none ?
		destination : vector_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(*left);
	stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] bool append_vector_map3_half(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode != 0x1d)
		return false;
	const auto source = vector_offset(instruction.source);
	if (!source)
		return false;
	const bool wide = instruction.source.width == OperandWidth::ymm;
	if (!wide && instruction.source.width != OperandWidth::xmm)
		return false;
	const bool memory = instruction.destination.kind == OperandKind::memory;
	const auto entry = memory ?
		(wide ? native::uml_x86_64_gadget_vector_map3_half_256_m :
			native::uml_x86_64_gadget_vector_map3_half_128_m) :
		(wide ? native::uml_x86_64_gadget_vector_map3_half_256_r :
			native::uml_x86_64_gadget_vector_map3_half_128_r);
	if (!memory) {
		const auto destination = vector_offset(instruction.destination);
		if (!destination)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *source));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.destination, *source);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] NativeEntry vector_map3_math_entry(std::uint8_t opcode,
	OperandWidth width, bool memory) noexcept
{
#define UML_X86_VECTOR_MAP3_MATH(symbol, bits) \
	return memory ? \
		native::uml_x86_64_gadget_vector_map3_math_##symbol##_##bits##_rm : \
		native::uml_x86_64_gadget_vector_map3_math_##symbol##_##bits##_rr
#define UML_X86_VECTOR_MAP3_MATH_CASE(value, symbol) \
	case value: \
		if (width == OperandWidth::xmm) \
			UML_X86_VECTOR_MAP3_MATH(symbol, 128); \
		if (width == OperandWidth::ymm) \
			UML_X86_VECTOR_MAP3_MATH(symbol, 256); \
		break
	switch (opcode) {
	UML_X86_VECTOR_MAP3_MATH_CASE(0x40, 40);
	UML_X86_VECTOR_MAP3_MATH_CASE(0x41, 41);
	UML_X86_VECTOR_MAP3_MATH_CASE(0x42, 42);
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_MATH_CASE
#undef UML_X86_VECTOR_MAP3_MATH
	return nullptr;
}

[[nodiscard]] bool append_vector_map3_math(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	const auto entry = vector_map3_math_entry(instruction.vector_opcode,
		instruction.destination.width, memory);
	if (entry == nullptr)
		return false;
	const auto destination = vector_offset(instruction.destination);
	const auto left = vector_offset(instruction.source);
	if (!destination || !left)
		return false;
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(*left);
	stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] bool append_vector_map3_crypto(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	if (opcode != 0x44 && opcode != 0xcc && opcode != 0xdf)
		return false;
	const auto destination = vector_offset(instruction.destination);
	if (!destination)
		return false;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	if (opcode == 0xdf) {
		const auto entry = memory ?
			native::uml_x86_64_gadget_vector_map3_aeskeygen_rm :
			native::uml_x86_64_gadget_vector_map3_aeskeygen_rr;
		if (!memory) {
			const auto input = vector_offset(instruction.auxiliary);
			if (!input)
				return false;
			stream.push_back(entry_address(entry));
			stream.push_back(pack_offsets(*destination, *input) |
				vex_tag(instruction));
			stream.push_back(instruction.vector_control);
			return true;
		}
		const auto encoded = encode_memory(
			gadget, instruction.auxiliary, *destination);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget, DescriptorTag::vex);
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto left = vector_offset(instruction.source);
	if (!left)
		return false;
	NativeEntry entry{};
	if (opcode == 0xcc) {
		entry = memory ?
			native::uml_x86_64_gadget_vector_map3_sha1rnds4_rm :
			native::uml_x86_64_gadget_vector_map3_sha1rnds4_rr;
	} else if (instruction.destination.width == OperandWidth::xmm) {
		entry = memory ? native::uml_x86_64_gadget_vector_map3_pclmul_128_rm :
			native::uml_x86_64_gadget_vector_map3_pclmul_128_rr;
	} else if (instruction.destination.width == OperandWidth::ymm) {
		entry = memory ? native::uml_x86_64_gadget_vector_map3_pclmul_256_rm :
			native::uml_x86_64_gadget_vector_map3_pclmul_256_rr;
	} else {
		return false;
	}
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*destination, *left, *right) |
			vex_tag(instruction));
		stream.push_back(instruction.vector_control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *destination);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(*left);
	stream.push_back(instruction.vector_control);
	return true;
}

[[nodiscard]] bool append_vector_map3_string(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	if (instruction.vector_opcode < 0x60 ||
		instruction.vector_opcode > 0x63)
		return false;
	const auto left = vector_offset(instruction.source);
	if (!left)
		return false;
	const bool memory = instruction.auxiliary.kind == OperandKind::memory;
	NativeEntry entry{};
#define UML_X86_VECTOR_MAP3_STRING_CASE(value, symbol) \
	case value: entry = memory ? \
		native::uml_x86_64_gadget_vector_map3_string_##symbol##_rm : \
		native::uml_x86_64_gadget_vector_map3_string_##symbol##_rr; break
	switch (instruction.vector_opcode) {
	UML_X86_VECTOR_MAP3_STRING_CASE(0x60, 60);
	UML_X86_VECTOR_MAP3_STRING_CASE(0x61, 61);
	UML_X86_VECTOR_MAP3_STRING_CASE(0x62, 62);
	UML_X86_VECTOR_MAP3_STRING_CASE(0x63, 63);
	default: break;
	}
#undef UML_X86_VECTOR_MAP3_STRING_CASE
	const auto control = static_cast<std::uintptr_t>(
		instruction.vector_control) |
		(static_cast<std::uintptr_t>(instruction.vector_group) << 8U);
	if (!memory) {
		const auto right = vector_offset(instruction.auxiliary);
		if (!right)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(pack_offsets(*left, *right) |
			vex_tag(instruction));
		stream.push_back(control);
		return true;
	}
	const auto encoded = encode_memory(
		gadget, instruction.auxiliary, *left);
	if (!encoded)
		return false;
	append_memory_words(stream, entry_address(entry), *encoded, gadget,
		DescriptorTag::vex);
	stream.push_back(control);
	return true;
}

[[nodiscard]] bool append_vector_integer_map3(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	if (append_vector_map3_shuffle(stream, gadget))
		return true;
	if (append_vector_map3_round(stream, gadget))
		return true;
	if (append_vector_map3_half(stream, gadget))
		return true;
	if (append_vector_map3_math(stream, gadget))
		return true;
	if (append_vector_map3_crypto(stream, gadget))
		return true;
	if (append_vector_map3_string(stream, gadget))
		return true;
	if (append_vector_map3_extract_scalar(stream, gadget))
		return true;
	if (append_vector_map3_extract_lane(stream, gadget))
		return true;
	if (append_vector_map3_insert_scalar(stream, gadget))
		return true;
	return append_vector_map3_insertps(stream, gadget);
}

[[nodiscard]] std::uintptr_t extension_entry(bool sign,
	OperandWidth source, OperandWidth destination) noexcept
{
#define UML_X86_EXTENSION(source_bits, destination_bits) \
	return entry_address(sign ? \
		native::uml_x86_64_gadget_sext_##source_bits##_##destination_bits##_rr : \
		native::uml_x86_64_gadget_zext_##source_bits##_##destination_bits##_rr)
	if (source == OperandWidth::byte && destination == OperandWidth::word)
		UML_X86_EXTENSION(8, 16);
	if (source == OperandWidth::byte && destination == OperandWidth::dword)
		UML_X86_EXTENSION(8, 32);
	if (source == OperandWidth::byte && destination == OperandWidth::qword)
		UML_X86_EXTENSION(8, 64);
	if (source == OperandWidth::word && destination == OperandWidth::dword)
		UML_X86_EXTENSION(16, 32);
	if (source == OperandWidth::word && destination == OperandWidth::qword)
		UML_X86_EXTENSION(16, 64);
	if (sign && source == OperandWidth::dword &&
		destination == OperandWidth::word)
		return entry_address(native::uml_x86_64_gadget_sext_32_16_rr);
	if (sign && source == OperandWidth::dword &&
		destination == OperandWidth::dword)
		return entry_address(native::uml_x86_64_gadget_sext_32_32_rr);
	if (source == OperandWidth::dword && destination == OperandWidth::qword)
		UML_X86_EXTENSION(32, 64);
#undef UML_X86_EXTENSION
	return 0;
}

[[nodiscard]] std::uintptr_t memory_extension_entry(bool sign,
	OperandWidth source, OperandWidth destination) noexcept
{
#define UML_X86_MEMORY_EXTENSION(source_bits, destination_bits) \
	return entry_address(sign ? \
		native::uml_x86_64_gadget_sext_##source_bits##_##destination_bits##_rm : \
		native::uml_x86_64_gadget_zext_##source_bits##_##destination_bits##_rm)
	if (source == OperandWidth::byte && destination == OperandWidth::word)
		UML_X86_MEMORY_EXTENSION(8, 16);
	if (source == OperandWidth::byte && destination == OperandWidth::dword)
		UML_X86_MEMORY_EXTENSION(8, 32);
	if (source == OperandWidth::byte && destination == OperandWidth::qword)
		UML_X86_MEMORY_EXTENSION(8, 64);
	if (source == OperandWidth::word && destination == OperandWidth::dword)
		UML_X86_MEMORY_EXTENSION(16, 32);
	if (source == OperandWidth::word && destination == OperandWidth::qword)
		UML_X86_MEMORY_EXTENSION(16, 64);
	if (sign && source == OperandWidth::dword &&
		destination == OperandWidth::word)
		return entry_address(native::uml_x86_64_gadget_sext_32_16_rm);
	if (sign && source == OperandWidth::dword &&
		destination == OperandWidth::dword)
		return entry_address(native::uml_x86_64_gadget_sext_32_32_rm);
	if (source == OperandWidth::dword && destination == OperandWidth::qword)
		UML_X86_MEMORY_EXTENSION(32, 64);
#undef UML_X86_MEMORY_EXTENSION
	return 0;
}

[[nodiscard]] bool append_memory_extension(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto destination = register_offset(instruction.destination);
	const auto encoded = destination ? encode_memory(
		gadget, instruction.source, *destination) : std::nullopt;
	const auto entry = memory_extension_entry(
		instruction.operation == Operation::move_sign_extend,
		instruction.source.width, instruction.destination.width);
	if (!encoded || entry == 0)
		return false;
	append_memory_words(stream, entry, *encoded, gadget);
	return true;
}

#define UML_X86_CONDITION_CASES(select) \
	select(overflow, o) \
	select(not_overflow, no) \
	select(below, b) \
	select(above_or_equal, ae) \
	select(equal, e) \
	select(not_equal, ne) \
	select(below_or_equal, be) \
	select(above, a) \
	select(sign, s) \
	select(not_sign, ns) \
	select(parity, p) \
	select(not_parity, np) \
	select(less, l) \
	select(greater_or_equal, ge) \
	select(less_or_equal, le) \
	select(greater, g)


/* Which host flags a gadget leaves behind for the next slot to read.
 * A logical operation clears C and V exactly as x86 clears CF and OF, so its
 * sign, zero and signed-order conditions transfer unchanged; the conditions
 * that read CF do not, because a cleared ARM C reads as "borrowed".
 * The narrow widths arrive zero-extended into a host register, so their host
 * flags describe a 32-bit operation. Zero still transfers, and subtraction
 * also preserves unsigned ordering because both operands are zero-extended.
 */
enum class HostFlags : std::uint8_t {
	none,
	zero_only,
	narrow_subtract,
	subtract,
	logical,
	/* An add's carry out is CF and its V is OF. Host C after an addition
	 * is set on carry, the opposite sense of the borrow after a
	 * subtraction, so BELOW and ABOVE_OR_EQUAL read the other's host
	 * condition and the two ordered carry conditions have no host form. */
	add,
	/* INC, DEC and NEG: N, Z and V describe the guest operation, C does not
	 * (preserved by INC and DEC, inverted by NEG), so the carry conditions
	 * stay on the deferred path exactly as after a logical operation. */
	no_carry,
};

/* Where a consumer that preserves the carry flag can take it from. INC and
 * DEC preserve CF, so the flag the previous producer defined has to reach
 * RFLAGS before they record their own; when the builder knows what that
 * producer left, the carry is one condition away and the call goes. */
enum class CarrySource : std::uint8_t {
	unknown,   // ask the materializer
	settled,   // RFLAGS already holds the carry and no override is pending
	subtract,  // NZCV describes a subtraction: x86 CF is the inverted borrow
	add,       // NZCV describes an addition: x86 CF is the carry out
	zero,      // a logical operation defined CF as zero
};

struct FlagState final {
	HostFlags condition{HostFlags::none};
	CarrySource carry{CarrySource::unknown};
	VectorResult vector{};
};

[[nodiscard]] CarrySource carry_of(HostFlags flags) noexcept
{
	switch (flags) {
	case HostFlags::subtract: return CarrySource::subtract;
	case HostFlags::add: return CarrySource::add;
	case HostFlags::logical: return CarrySource::zero;
	default: return CarrySource::unknown;
	}
}

/* The host flags an ordinary-bank gadget leaves; the baked bank reports its
 * own through append_baked. */
[[nodiscard]] HostFlags host_flags_of(const Instruction &instruction) noexcept
{
	if (instruction.destination.kind == OperandKind::memory ||
		instruction.source.kind == OperandKind::memory)
		return HostFlags::none;
	const bool narrow = instruction.destination.width == OperandWidth::byte ||
		instruction.destination.width == OperandWidth::word;
	switch (instruction.operation) {
	case Operation::add:
		/* A narrow sum carries into bit 8 or 16 of the host result, so not
		 * even its zero condition survives in the host flags. */
		return narrow ? HostFlags::none : HostFlags::add;
	case Operation::subtract:
	case Operation::compare:
		return narrow ? HostFlags::narrow_subtract : HostFlags::subtract;
	case Operation::bit_and:
	case Operation::test:
		return narrow ? HostFlags::zero_only : HostFlags::logical;
	case Operation::increment:
	case Operation::decrement:
	case Operation::negate:
		/* The narrow unary gadgets use the host form that sets no flags. */
		return !narrow && instruction.destination.kind == OperandKind::reg ?
			HostFlags::no_carry : HostFlags::none;
	default:
		return HostFlags::none;
	}
}

/* The ordinary bank's INC and DEC settle the carry themselves, at every
 * width, so a second one straight after needs no settlement of its own. */
[[nodiscard]] CarrySource ordinary_carry_of(
	const Instruction &instruction) noexcept
{
	if (instruction.destination.kind == OperandKind::reg &&
		(instruction.operation == Operation::increment ||
		 instruction.operation == Operation::decrement))
		return CarrySource::settled;
	return carry_of(host_flags_of(instruction));
}

[[nodiscard]] bool fused_condition(
	Condition condition, HostFlags flags) noexcept
{
	if (flags == HostFlags::none)
		return false;
	if (flags == HostFlags::zero_only)
		return condition == Condition::equal || condition == Condition::not_equal;
	switch (condition) {
	/* Parity has no representation in NZCV. */
	case Condition::parity:
	case Condition::not_parity:
		return false;
	case Condition::below:
	case Condition::above_or_equal:
		return flags == HostFlags::subtract ||
			flags == HostFlags::narrow_subtract || flags == HostFlags::add;
	case Condition::below_or_equal:
	case Condition::above:
		return flags == HostFlags::subtract || flags == HostFlags::narrow_subtract;
	case Condition::equal:
	case Condition::not_equal:
		return true;
	default:
		return flags != HostFlags::narrow_subtract;
	}
}

/* The condition whose fused consumer reads the producer's host flags
 * correctly: after an addition the carry conditions swap. */
[[nodiscard]] Condition fused_consumer_condition(
	Condition condition, HostFlags flags) noexcept
{
	if (flags != HostFlags::add)
		return condition;
	switch (condition) {
	case Condition::below: return Condition::above_or_equal;
	case Condition::above_or_equal: return Condition::below;
	default: return condition;
	}
}

#define UML_X86_FUSED_CONDITION_CASES(select) \
	select(overflow, o) \
	select(not_overflow, no) \
	select(below, b) \
	select(above_or_equal, ae) \
	select(equal, e) \
	select(not_equal, ne) \
	select(below_or_equal, be) \
	select(above, a) \
	select(sign, s) \
	select(not_sign, ns) \
	select(less, l) \
	select(greater_or_equal, ge) \
	select(less_or_equal, le) \
	select(greater, g)

[[nodiscard]] std::uintptr_t set_condition_fused_entry(
	Condition condition) noexcept
{
	switch (condition) {
#define UML_X86_SET_FUSED_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address(native::uml_x86_64_gadget_set_##name##_nzcv);
	UML_X86_FUSED_CONDITION_CASES(UML_X86_SET_FUSED_CASE)
#undef UML_X86_SET_FUSED_CASE
	default: return 0;
	}
}

[[nodiscard]] std::uintptr_t jump_condition_fused_entry(
	Condition condition, bool linked) noexcept
{
	switch (condition) {
#define UML_X86_JUMP_FUSED_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address(linked ? \
			native::uml_x86_64_gadget_jump_##name##_linked_nzcv : \
			native::uml_x86_64_gadget_jump_##name##_nzcv);
	UML_X86_FUSED_CONDITION_CASES(UML_X86_JUMP_FUSED_CASE)
#undef UML_X86_JUMP_FUSED_CASE
	default: return 0;
	}
}

[[nodiscard]] std::uintptr_t jump_condition_inline_fused_entry(
	Condition condition) noexcept
{
	switch (condition) {
#define UML_X86_JUMP_INLINE_FUSED_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address( \
			native::uml_x86_64_gadget_jump_##name##_inline_nzcv);
	UML_X86_FUSED_CONDITION_CASES(UML_X86_JUMP_INLINE_FUSED_CASE)
#undef UML_X86_JUMP_INLINE_FUSED_CASE
	default: return 0;
	}
}

[[nodiscard]] std::uintptr_t conditional_move_fused_entry(
	Condition condition, OperandWidth width) noexcept
{
#define UML_X86_CMOV_FUSED_WIDTH(name) \
	switch (width) { \
	case OperandWidth::word: \
		return entry_address( \
			native::uml_x86_64_gadget_cmov_##name##_16_rr_nzcv); \
	case OperandWidth::dword: \
		return entry_address( \
			native::uml_x86_64_gadget_cmov_##name##_32_rr_nzcv); \
	case OperandWidth::qword: \
		return entry_address( \
			native::uml_x86_64_gadget_cmov_##name##_64_rr_nzcv); \
	default: return 0; \
	}
	switch (condition) {
#define UML_X86_CMOV_FUSED_CASE(enumerator, name) \
	case Condition::enumerator: UML_X86_CMOV_FUSED_WIDTH(name);
	UML_X86_FUSED_CONDITION_CASES(UML_X86_CMOV_FUSED_CASE)
#undef UML_X86_CMOV_FUSED_CASE
	default: return 0;
	}
#undef UML_X86_CMOV_FUSED_WIDTH
}

[[nodiscard]] std::uintptr_t set_condition_entry(Condition condition) noexcept
{
	switch (condition) {
#define UML_X86_SET_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address(native::uml_x86_64_gadget_set_##name);
	UML_X86_CONDITION_CASES(UML_X86_SET_CASE)
#undef UML_X86_SET_CASE
	}
	return 0;
}

[[nodiscard]] std::uintptr_t set_condition_memory_entry(
	Condition condition) noexcept
{
	switch (condition) {
#define UML_X86_SET_MEMORY_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address(native::uml_x86_64_gadget_set_##name##_m);
	UML_X86_CONDITION_CASES(UML_X86_SET_MEMORY_CASE)
#undef UML_X86_SET_MEMORY_CASE
	}
	return 0;
}

[[nodiscard]] std::uintptr_t jump_condition_entry(
	Condition condition, bool linked) noexcept
{
	switch (condition) {
#define UML_X86_JUMP_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address(linked ? \
			native::uml_x86_64_gadget_jump_##name##_linked : \
			native::uml_x86_64_gadget_jump_##name);
	UML_X86_CONDITION_CASES(UML_X86_JUMP_CASE)
#undef UML_X86_JUMP_CASE
	}
	return 0;
}

[[nodiscard]] std::uintptr_t jump_condition_inline_entry(
	Condition condition) noexcept
{
	switch (condition) {
#define UML_X86_JUMP_INLINE_CASE(enumerator, name) \
	case Condition::enumerator: \
		return entry_address( \
			native::uml_x86_64_gadget_jump_##name##_inline);
	UML_X86_CONDITION_CASES(UML_X86_JUMP_INLINE_CASE)
#undef UML_X86_JUMP_INLINE_CASE
	}
	return 0;
}

[[nodiscard]] std::uintptr_t conditional_move_entry(
	Condition condition, OperandWidth width) noexcept
{
#define UML_X86_CMOV_WIDTH(name) \
	switch (width) { \
	case OperandWidth::word: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_16_rr); \
	case OperandWidth::dword: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_32_rr); \
	case OperandWidth::qword: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_64_rr); \
	default: return 0; \
	}
	switch (condition) {
#define UML_X86_CMOV_CASE(enumerator, name) \
	case Condition::enumerator: UML_X86_CMOV_WIDTH(name);
	UML_X86_CONDITION_CASES(UML_X86_CMOV_CASE)
#undef UML_X86_CMOV_CASE
	}
#undef UML_X86_CMOV_WIDTH
	return 0;
}

[[nodiscard]] std::uintptr_t conditional_move_memory_entry(
	Condition condition, OperandWidth width) noexcept
{
#define UML_X86_CMOV_MEMORY_WIDTH(name) \
	switch (width) { \
	case OperandWidth::word: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_16_rm); \
	case OperandWidth::dword: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_32_rm); \
	case OperandWidth::qword: \
		return entry_address(native::uml_x86_64_gadget_cmov_##name##_64_rm); \
	default: return 0; \
	}
	switch (condition) {
#define UML_X86_CMOV_MEMORY_CASE(enumerator, name) \
	case Condition::enumerator: UML_X86_CMOV_MEMORY_WIDTH(name);
	UML_X86_CONDITION_CASES(UML_X86_CMOV_MEMORY_CASE)
#undef UML_X86_CMOV_MEMORY_CASE
	}
#undef UML_X86_CMOV_MEMORY_WIDTH
	return 0;
}

#undef UML_X86_CONDITION_CASES

[[nodiscard]] bool append_register_pair(
	StreamWords &stream, std::uintptr_t entry,
	const Operand &destination, const Operand &source) noexcept
{
	const auto destination_offset = register_offset(destination);
	const auto source_offset = register_offset(source);
	if (entry == 0 || !destination_offset || !source_offset)
		return false;
	stream.push_back(entry);
	stream.push_back(pack_offsets(*destination_offset, *source_offset));
	return true;
}

[[nodiscard]] bool append_register_binary(
	StreamWords &stream, const Gadget &gadget,
	std::uintptr_t entry) noexcept
{
	const auto destination = register_offset(
		gadget.instruction.destination);
	if (!destination || entry == 0)
		return false;
	const auto &source_operand = gadget.instruction.source;
	if (source_operand.kind == OperandKind::immediate) {
		stream.push_back(entry);
		stream.push_back(*destination);
		stream.push_back(source_operand.immediate);
		return true;
	}
	const auto source = register_offset(source_operand);
	if (!source)
		return false;
	stream.push_back(entry);
	stream.push_back(pack_offsets(*destination, *source));
	return true;
}

[[nodiscard]] std::optional<std::uint16_t> segment_selector_offset(
	std::uint8_t index) noexcept
{
	if (index >= 6U)
		return std::nullopt;
	return static_cast<std::uint16_t>(
		offsetof(CpuState, segment_selectors) + index * sizeof(std::uint16_t));
}

/* MOV from a segment register; a load of one is the segment model's. */
[[nodiscard]] bool append_segment_move(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto selector = segment_selector_offset(instruction.vector_group);
	if (!selector || instruction.vector_opcode == 0x8e)
		return false;
	if (instruction.destination.kind == OperandKind::memory) {
		const auto encoded = encode_memory(
			gadget, instruction.destination, *selector);
		if (!encoded || instruction.destination.width != OperandWidth::word)
			return false;
		append_memory_words(stream,
			entry_address(native::uml_x86_64_gadget_move_16_mr),
			*encoded, gadget);
		return true;
	}
	const auto destination = register_offset(instruction.destination);
	if (!destination)
		return false;
	std::uintptr_t entry{};
	switch (instruction.destination.width) {
	case OperandWidth::word:
		entry = entry_address(native::uml_x86_64_gadget_move_16_rr);
		break;
	case OperandWidth::dword:
		entry = entry_address(native::uml_x86_64_gadget_zext_16_32_rr);
		break;
	case OperandWidth::qword:
		entry = entry_address(native::uml_x86_64_gadget_zext_16_64_rr);
		break;
	default: return false;
	}
	stream.push_back(entry);
	stream.push_back(pack_offsets(*destination, *selector));
	return true;
}

/* PUSH of a segment register; a POP to one is the segment model's. */
[[nodiscard]] bool append_segment_stack(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto selector = segment_selector_offset(instruction.vector_group);
	const auto width = instruction.source.width;
	if (!selector || instruction.vector_control == 0 ||
		(width != OperandWidth::word && width != OperandWidth::dword &&
			width != OperandWidth::qword))
		return false;
	const NativeEntry entry = width == OperandWidth::word ?
		native::uml_x86_64_gadget_push_16_r :
		width == OperandWidth::dword ?
			native::uml_x86_64_gadget_push_segment_32 :
			native::uml_x86_64_gadget_push_segment_64;
	stream.push_back(entry_address(entry));
	stream.push_back(static_cast<std::uintptr_t>(*selector) |
		stream.locator(gadget));
	return true;
}

[[nodiscard]] bool append_enter(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	NativeEntry entry{};
	if (instruction.destination.width == OperandWidth::word)
		entry = native::uml_x86_64_gadget_enter_16;
	else if (instruction.destination.width == OperandWidth::dword)
		entry = native::uml_x86_64_gadget_enter_32;
	else if (instruction.destination.width == OperandWidth::qword)
		entry = native::uml_x86_64_gadget_enter_64;
	else
		return false;
	const auto descriptor =
		(static_cast<std::uintptr_t>(instruction.auxiliary.immediate) & 0x1fU) |
		((static_cast<std::uintptr_t>(instruction.source.immediate) & 0xffffU)
			<< 16U) |
		stream.locator(gadget);
	stream.push_back(entry_address(entry));
	stream.push_back(descriptor);
	return true;
}

/* 64-bit mode pops SS:RSP at every operand size, compat32 only on a return
 * to an outer privilege level, so the narrower forms come in both kinds. */
[[nodiscard]] NativeEntry interrupt_return_entry(OperandWidth width,
	ExecutionMode mode) noexcept
{
	const bool long_mode = mode == ExecutionMode::long64;
	switch (width) {
	case OperandWidth::word:
		return long_mode ?
			native::uml_x86_64_gadget_interrupt_return_long_16 :
			native::uml_x86_64_gadget_interrupt_return_16;
	case OperandWidth::dword:
		return long_mode ?
			native::uml_x86_64_gadget_interrupt_return_long_32 :
			native::uml_x86_64_gadget_interrupt_return_32;
	case OperandWidth::qword:
		return long_mode ?
			native::uml_x86_64_gadget_interrupt_return_64 : nullptr;
	default: return nullptr;
	}
}

[[nodiscard]] NativeEntry string_entry(
	Operation operation, OperandWidth width, bool step = false) noexcept
{
#define UML_X86_STRING_WIDTH(operation_name) \
	switch (width) { \
	case OperandWidth::byte: \
		return step ? \
			native::uml_x86_64_gadget_string_##operation_name##_8_step : \
			native::uml_x86_64_gadget_string_##operation_name##_8; \
	case OperandWidth::word: \
		return step ? \
			native::uml_x86_64_gadget_string_##operation_name##_16_step : \
			native::uml_x86_64_gadget_string_##operation_name##_16; \
	case OperandWidth::dword: \
		return step ? \
			native::uml_x86_64_gadget_string_##operation_name##_32_step : \
			native::uml_x86_64_gadget_string_##operation_name##_32; \
	case OperandWidth::qword: \
		return step ? \
			native::uml_x86_64_gadget_string_##operation_name##_64_step : \
			native::uml_x86_64_gadget_string_##operation_name##_64; \
	default: return nullptr; \
	}
	switch (operation) {
	case Operation::string_move: UML_X86_STRING_WIDTH(move);
	case Operation::string_compare: UML_X86_STRING_WIDTH(compare);
	case Operation::string_store: UML_X86_STRING_WIDTH(store);
	case Operation::string_load: UML_X86_STRING_WIDTH(load);
	case Operation::string_scan: UML_X86_STRING_WIDTH(scan);
	default: return nullptr;
	}
#undef UML_X86_STRING_WIDTH
}

[[nodiscard]] bool append_string(StreamWords &stream,
	const Gadget &gadget, bool step = false) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto width = instruction.destination.kind == OperandKind::none ?
		instruction.source.width : instruction.destination.width;
	const auto entry = string_entry(instruction.operation, width, step);
	const auto *memory = instruction.destination.kind == OperandKind::memory ?
		&instruction.destination : instruction.source.kind == OperandKind::memory ?
		&instruction.source : nullptr;
	if (entry == nullptr || memory == nullptr)
		return false;
	Segment source_segment{Segment::none};
	if (instruction.operation == Operation::string_move ||
		instruction.operation == Operation::string_load)
		source_segment = instruction.source.memory.segment;
	else if (instruction.operation == Operation::string_compare)
		source_segment = instruction.destination.memory.segment;
	const auto descriptor =
		static_cast<std::uintptr_t>(memory->memory.address_32) |
		(static_cast<std::uintptr_t>(source_segment) << 1U) |
		(static_cast<std::uintptr_t>(memory->memory.address_16) << 3U) |
		(static_cast<std::uintptr_t>(instruction.repeat) << 8U) |
		stream.locator(gadget);
	stream.push_back(entry_address(entry));
	stream.push_back(descriptor);
	return true;
}

[[nodiscard]] bool append_extended_state(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto encoded = encode_memory(gadget, instruction.destination, 0);
	if (!encoded)
		return false;
	NativeEntry entry{};
	switch (instruction.vector_group) {
	case 0: entry = native::uml_x86_64_gadget_extended_state_fxsave; break;
	case 1: entry = native::uml_x86_64_gadget_extended_state_fxrstor; break;
	case 4: entry = native::uml_x86_64_gadget_extended_state_xsave; break;
	case 5: entry = native::uml_x86_64_gadget_extended_state_xrstor; break;
	default: return false;
	}
	append_memory_words(stream, entry_address(entry), *encoded, gadget);
	return true;
}

[[nodiscard]] bool append_x87(
	StreamWords &stream, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const auto modrm = instruction.vector_control;
	const auto group = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
	const auto index = static_cast<std::uint8_t>(modrm & 7U);
	NativeEntry entry{};
	bool carries_index{};
	if (opcode == 0x77) {
		entry = native::uml_x86_64_gadget_x87_emms;
	} else if ((modrm >> 6U) != 3) {
		const auto encoded = encode_memory(gadget, instruction.source, 0);
		if (!encoded)
			return false;
		if (opcode == 0xd9 && group == 5) {
			entry = native::uml_x86_64_gadget_x87_load_control;
		} else if (opcode == 0xd9 && group == 7) {
			entry = native::uml_x86_64_gadget_x87_store_control;
		} else if (opcode == 0xdd && group == 7) {
			entry = native::uml_x86_64_gadget_x87_store_status;
		} else if ((opcode == 0xd9 || opcode == 0xdd) &&
			(group == 4 || group == 6)) {
			const bool compact = instruction.vector_prefix != 0;
			const bool full = opcode == 0xdd;
			const bool restore = group == 4;
			if (restore && full)
				entry = compact ?
					native::uml_x86_64_gadget_x87_environment_restore_state94 :
					native::uml_x86_64_gadget_x87_environment_restore_state108;
			else if (restore)
				entry = compact ?
					native::uml_x86_64_gadget_x87_environment_restore_env14 :
					native::uml_x86_64_gadget_x87_environment_restore_env28;
			else if (full)
				entry = compact ?
					native::uml_x86_64_gadget_x87_environment_save_state94 :
					native::uml_x86_64_gadget_x87_environment_save_state108;
			else
				entry = compact ?
					native::uml_x86_64_gadget_x87_environment_save_env14 :
					native::uml_x86_64_gadget_x87_environment_save_env28;
		} else if (opcode == 0xd9 && group == 0) {
			entry = native::uml_x86_64_gadget_x87_load_f32;
		} else if (opcode == 0xdb && group == 0) {
			entry = native::uml_x86_64_gadget_x87_load_i32;
		} else if (opcode == 0xdb && group == 5) {
			entry = native::uml_x86_64_gadget_x87_load_f80;
		} else if (opcode == 0xdd && group == 0) {
			entry = native::uml_x86_64_gadget_x87_load_f64;
		} else if (opcode == 0xdf && group == 0) {
			entry = native::uml_x86_64_gadget_x87_load_i16;
		} else if (opcode == 0xdf && group == 4) {
			entry = native::uml_x86_64_gadget_x87_load_bcd;
		} else if (opcode == 0xdf && group == 5) {
			entry = native::uml_x86_64_gadget_x87_load_i64;
		} else if (opcode == 0xd9 && group == 2) {
			entry = native::uml_x86_64_gadget_x87_store_f32_keep;
		} else if (opcode == 0xd9 && group == 3) {
			entry = native::uml_x86_64_gadget_x87_store_f32_pop;
		} else if (opcode == 0xdb && group == 1) {
			entry = native::uml_x86_64_gadget_x87_store_i32_truncate_pop;
		} else if (opcode == 0xdb && group == 2) {
			entry = native::uml_x86_64_gadget_x87_store_i32_keep;
		} else if (opcode == 0xdb && group == 3) {
			entry = native::uml_x86_64_gadget_x87_store_i32_pop;
		} else if (opcode == 0xdb && group == 7) {
			entry = native::uml_x86_64_gadget_x87_store_f80_pop;
		} else if (opcode == 0xdd && group == 1) {
			entry = native::uml_x86_64_gadget_x87_store_i64_truncate_pop;
		} else if (opcode == 0xdd && group == 2) {
			entry = native::uml_x86_64_gadget_x87_store_f64_keep;
		} else if (opcode == 0xdd && group == 3) {
			entry = native::uml_x86_64_gadget_x87_store_f64_pop;
		} else if (opcode == 0xdf && group == 1) {
			entry = native::uml_x86_64_gadget_x87_store_i16_truncate_pop;
		} else if (opcode == 0xdf && group == 2) {
			entry = native::uml_x86_64_gadget_x87_store_i16_keep;
		} else if (opcode == 0xdf && group == 3) {
			entry = native::uml_x86_64_gadget_x87_store_i16_pop;
		} else if (opcode == 0xdf && group == 6) {
			entry = native::uml_x86_64_gadget_x87_store_bcd_pop;
		} else if (opcode == 0xdf && group == 7) {
			entry = native::uml_x86_64_gadget_x87_store_i64_pop;
		} else if (opcode == 0xd9 || opcode == 0xdb || opcode == 0xdd) {
			/* D9 /1, DB /4, DB /6 and DD /5 are undefined. */
			return false;
		} else {
			const auto select = [group](NativeEntry add, NativeEntry multiply,
				NativeEntry compare_keep, NativeEntry compare_pop,
				NativeEntry subtract, NativeEntry subtract_reverse,
				NativeEntry divide, NativeEntry divide_reverse) noexcept {
				switch (group) {
				case 0: return add;
				case 1: return multiply;
				case 2: return compare_keep;
				case 3: return compare_pop;
				case 4: return subtract;
				case 5: return subtract_reverse;
				case 6: return divide;
				case 7: return divide_reverse;
				default: return NativeEntry{};
				}
			};
#define UML_X86_SELECT_X87_MEMORY(type) select( \
			native::uml_x86_64_gadget_x87_memory_##type##_add, \
			native::uml_x86_64_gadget_x87_memory_##type##_multiply, \
			native::uml_x86_64_gadget_x87_memory_##type##_compare_0, \
			native::uml_x86_64_gadget_x87_memory_##type##_compare_1, \
			native::uml_x86_64_gadget_x87_memory_##type##_subtract, \
			native::uml_x86_64_gadget_x87_memory_##type##_subtract_reverse, \
			native::uml_x86_64_gadget_x87_memory_##type##_divide, \
			native::uml_x86_64_gadget_x87_memory_##type##_divide_reverse)
			if (opcode == 0xd8 || opcode == 0xd9)
				entry = UML_X86_SELECT_X87_MEMORY(f32);
			else if (opcode == 0xda || opcode == 0xdb)
				entry = UML_X86_SELECT_X87_MEMORY(i32);
			else if (opcode == 0xdc || opcode == 0xdd)
				entry = UML_X86_SELECT_X87_MEMORY(f64);
			else
				entry = UML_X86_SELECT_X87_MEMORY(i16);
#undef UML_X86_SELECT_X87_MEMORY
		}
		append_memory_words(stream, entry_address(entry), *encoded,
			gadget);
		return true;
	} else if (opcode == 0xd8 || opcode == 0xdc || opcode == 0xde) {
		if ((opcode == 0xd8 || opcode == 0xdc) &&
			(group == 2 || group == 3)) {
			entry = group == 2 ?
				native::uml_x86_64_gadget_x87_compare_status_keep :
				native::uml_x86_64_gadget_x87_compare_status_pop;
		} else if (opcode == 0xde && group == 2) {
			entry = native::uml_x86_64_gadget_x87_compare_status_pop;
		} else if (opcode == 0xde && group == 3) {
			if (index != 1)
				return false;
			entry = native::uml_x86_64_gadget_x87_compare_status_pop2;
		} else {
			const auto select = [group](const std::array<NativeEntry, 8> &entries) {
				return entries[group];
			};
#define UML_X86_X87_REGISTER_ENTRIES(opcode_name) \
			std::array<NativeEntry, 8>{ \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_0, \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_1, \
				nullptr, nullptr, \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_4, \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_5, \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_6, \
				native::uml_x86_64_gadget_x87_register_##opcode_name##_7, \
			}
			if (opcode == 0xd8)
				entry = select(UML_X86_X87_REGISTER_ENTRIES(d8));
			else if (opcode == 0xdc)
				entry = select(UML_X86_X87_REGISTER_ENTRIES(dc));
			else
				entry = select(UML_X86_X87_REGISTER_ENTRIES(de));
#undef UML_X86_X87_REGISTER_ENTRIES
			if (entry == nullptr)
				return false;
		}
		carries_index = true;
	} else if (opcode == 0xd9) {
		if (group == 0) {
			entry = native::uml_x86_64_gadget_x87_load_register;
			carries_index = true;
		} else if (group == 3) {
			/* D9 D8+i is an alias of FSTP ST(i). */
			entry = native::uml_x86_64_gadget_x87_store_register_pop;
			carries_index = true;
		} else if (group == 1) {
			entry = native::uml_x86_64_gadget_x87_exchange_register;
			carries_index = true;
		} else if (modrm == 0xd0) {
			entry = native::uml_x86_64_gadget_x87_nop;
		} else if (modrm == 0xe0) {
			entry = native::uml_x86_64_gadget_x87_change_sign;
		} else if (modrm == 0xe1) {
			entry = native::uml_x86_64_gadget_x87_absolute;
		} else if (modrm == 0xe4) {
			entry = native::uml_x86_64_gadget_x87_test_zero;
		} else if (modrm == 0xe5) {
			entry = native::uml_x86_64_gadget_x87_examine;
		} else if (modrm >= 0xe8 && modrm <= 0xee) {
			constexpr std::array<NativeEntry, 7> constants{
				native::uml_x86_64_gadget_x87_constant_one,
				native::uml_x86_64_gadget_x87_constant_log2_10,
				native::uml_x86_64_gadget_x87_constant_log2_e,
				native::uml_x86_64_gadget_x87_constant_pi,
				native::uml_x86_64_gadget_x87_constant_log10_2,
				native::uml_x86_64_gadget_x87_constant_ln_2,
				native::uml_x86_64_gadget_x87_constant_zero,
			};
			entry = constants[modrm - 0xe8U];
		} else if (modrm == 0xf0) {
			entry = native::uml_x86_64_gadget_x87_f2xm1;
		} else if (modrm == 0xf1) {
			entry = native::uml_x86_64_gadget_x87_fyl2x;
		} else if (modrm == 0xf2) {
			entry = native::uml_x86_64_gadget_x87_tangent;
		} else if (modrm == 0xf3) {
			entry = native::uml_x86_64_gadget_x87_atan;
		} else if (modrm == 0xf4) {
			entry = native::uml_x86_64_gadget_x87_extract;
		} else if (modrm == 0xf5) {
			entry = native::uml_x86_64_gadget_x87_fprem1_with_flags;
		} else if (modrm == 0xf6) {
			entry = native::uml_x86_64_gadget_x87_decrement_top;
		} else if (modrm == 0xf7) {
			entry = native::uml_x86_64_gadget_x87_increment_top;
		} else if (modrm == 0xf8) {
			entry = native::uml_x86_64_gadget_x87_fprem_with_flags;
		} else if (modrm == 0xf9) {
			entry = native::uml_x86_64_gadget_x87_fyl2xp1;
		} else if (modrm == 0xfa) {
			entry = native::uml_x86_64_gadget_x87_sqrt;
		} else if (modrm == 0xfb) {
			entry = native::uml_x86_64_gadget_x87_sincos;
		} else if (modrm == 0xfc) {
			entry = native::uml_x86_64_gadget_x87_round;
		} else if (modrm == 0xfd) {
			entry = native::uml_x86_64_gadget_x87_scale;
		} else if (modrm == 0xfe) {
			entry = native::uml_x86_64_gadget_x87_sine;
		} else if (modrm == 0xff) {
			entry = native::uml_x86_64_gadget_x87_cosine;
		} else {
			return false;
		}
	} else if ((opcode == 0xda || opcode == 0xdb) && group <= 3) {
		constexpr std::array<NativeEntry, 8> conditions{
			native::uml_x86_64_gadget_x87_conditional_move_0,
			native::uml_x86_64_gadget_x87_conditional_move_1,
			native::uml_x86_64_gadget_x87_conditional_move_2,
			native::uml_x86_64_gadget_x87_conditional_move_3,
			native::uml_x86_64_gadget_x87_conditional_move_4,
			native::uml_x86_64_gadget_x87_conditional_move_5,
			native::uml_x86_64_gadget_x87_conditional_move_6,
			native::uml_x86_64_gadget_x87_conditional_move_7,
		};
		entry = conditions[group + (opcode == 0xdb ? 4U : 0U)];
		carries_index = true;
	} else if (opcode == 0xda && modrm == 0xe9) {
		entry = native::uml_x86_64_gadget_x87_compare_status_pop2;
		carries_index = true;
	} else if (opcode == 0xdb && modrm == 0xe2) {
		entry = native::uml_x86_64_gadget_x87_clear_exceptions;
	} else if (opcode == 0xdb && modrm == 0xe3) {
		entry = native::uml_x86_64_gadget_x87_initialize;
	} else if (opcode == 0xdb &&
		(modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe4)) {
		/* FNENI, FNDISI and FNSETPM do nothing since the 80387. */
		entry = native::uml_x86_64_gadget_x87_nop;
	} else if (opcode == 0xdb && (group == 5 || group == 6)) {
		entry = native::uml_x86_64_gadget_x87_compare_integer_keep;
		carries_index = true;
	} else if (opcode == 0xdd || opcode == 0xdf) {
		if (group == 0) {
			entry = opcode == 0xdf ?
				native::uml_x86_64_gadget_x87_free_register_pop :
				native::uml_x86_64_gadget_x87_free_register;
			carries_index = true;
		} else if (group == 1) {
			entry = native::uml_x86_64_gadget_x87_exchange_register;
			carries_index = true;
		} else if (group == 2 || group == 3) {
			entry = group == 3 || opcode == 0xdf ?
				native::uml_x86_64_gadget_x87_store_register_pop :
				native::uml_x86_64_gadget_x87_store_register_keep;
			carries_index = true;
		} else if (opcode == 0xdf && modrm == 0xe0) {
			entry = native::uml_x86_64_gadget_x87_store_status_ax;
		} else if (opcode == 0xdd && (group == 4 || group == 5)) {
			entry = group == 4 ?
				native::uml_x86_64_gadget_x87_compare_status_keep :
				native::uml_x86_64_gadget_x87_compare_status_pop;
			carries_index = true;
		} else if (opcode == 0xdf && (group == 5 || group == 6)) {
			entry = native::uml_x86_64_gadget_x87_compare_integer_pop;
			carries_index = true;
		} else {
			return false;
		}
	} else {
		return false;
	}
	/* FUCOM, FUCOMP, FUCOMPP, FUCOMI and FUCOMIP, which raise invalid
	 * operation only for a signaling NaN, mark the register word. */
	const bool quiet_compare = (opcode == 0xdd && (group == 4 || group == 5)) ||
		(opcode == 0xda && modrm == 0xe9) ||
		((opcode == 0xdb || opcode == 0xdf) && group == 5);
	stream.push_back(entry_address(entry));
	if (carries_index)
		stream.push_back(index | (quiet_compare ? 0x100U : 0U));
	return true;
}

/* ---- the register-baked scalar bank -------------------------------------
 *
 * These select an entry of engine/generate_baked.py's bank for the shapes it
 * enumerates and decline everything else, which then takes the ordinary
 * family. A baked entry reads no operand word: a register pair is the entry
 * alone, an immediate follows it, and a [base + displacement] access carries
 * one access word, described below. An indexed address is an address entry
 * with the access word, then the access entry, which finds the word in x23.
 */

[[nodiscard]] bool baked_bank_enabled() noexcept
{
	static const bool enabled = []() noexcept {
		const auto *value = std::getenv("UML_X86_64_GADGET_BAKED");
		return value == nullptr || value[0] == '\0' || value[0] != '0';
	}();
	return enabled;
}

[[nodiscard]] std::optional<unsigned> baked_register_slot(
	const Operand &operand) noexcept
{
	if (operand.kind != OperandKind::reg || operand.reg.index >= 16U)
		return std::nullopt;
	if (operand.width == OperandWidth::byte && operand.reg.high_byte) {
		if (operand.reg.index >= 4U)
			return std::nullopt;
		return baked::gpr_slots + operand.reg.index;
	}
	if (operand.reg.high_byte)
		return std::nullopt;
	return operand.reg.index;
}

[[nodiscard]] constexpr unsigned baked_value_slots(OperandWidth width) noexcept
{
	return width == OperandWidth::byte ? baked::byte_slots : baked::gpr_slots;
}

/* A baked access carries everything its entry needs of the instruction in one
 * access word:
 *
 *   bits 31:0   the displacement, which the entry sign-extends
 *   bits 41:32  the instruction's position in its block, its address less the
 *               block's start, which is the context's program_begin
 *   bits 55:42  a memory,immediate access's immediate, signed
 *   bits 59:56  the instruction's length
 *   bit  60     the store's publication flag, clear in the stream
 *
 * Only a fault or an exit after a store into the running block reads the
 * position back, and the cold exits rebuild the RIP from the block's start.
 * A block's start is the one every stream of it agrees on, the cached
 * translation's, a budget-limited replay's and a checked instruction's, so
 * the builder takes it as an argument rather than reading it off the first
 * instruction it was given. An instruction 1024 bytes or more past it, which
 * a block of the default 64 instructions never holds, takes the ordinary
 * bank.
 *
 * A fused access with no base register still needs a 64-bit address, a
 * RIP-relative or absolute one, so its stream carries the address before the
 * word, whose displacement field is clear.
 *
 * PUSH and POP of a register have no access word in a block's own stream
 * (`append_baked_stack`).
 */
constexpr unsigned baked_position_shift = 32;
constexpr unsigned baked_position_bits = 10;
constexpr unsigned baked_immediate_shift = 42;
constexpr unsigned baked_immediate_bits = 14;
constexpr unsigned baked_length_shift = 56;

/* The access word without its displacement and immediate, or nothing when the
 * instruction lies too far into its block. */
[[nodiscard]] std::optional<std::uintptr_t> baked_access_header(
	const Gadget &gadget, std::uint64_t program_begin) noexcept
{
	const auto position = gadget.address - program_begin;
	if (position >= (std::uint64_t{1} << baked_position_bits))
		return std::nullopt;
	return static_cast<std::uintptr_t>(position << baked_position_shift) |
		(static_cast<std::uintptr_t>(gadget.instruction.length) <<
			baked_length_shift);
}

/* A memory,immediate access's immediate as the access word holds it: the
 * value the operation sees at its width, sign-extended, when that fits. The
 * entries sign-extend it again, and every consumer reads the bits of its
 * width. */
[[nodiscard]] std::optional<std::uintptr_t> baked_packed_immediate(
	std::uint64_t immediate, OperandWidth width) noexcept
{
	std::int64_t value{};
	switch (width) {
	case OperandWidth::byte: value = static_cast<std::int8_t>(immediate); break;
	case OperandWidth::word: value = static_cast<std::int16_t>(immediate); break;
	case OperandWidth::dword: value = static_cast<std::int32_t>(immediate); break;
	default: value = static_cast<std::int64_t>(immediate); break;
	}
	constexpr std::int64_t limit = std::int64_t{1} << (baked_immediate_bits - 1U);
	if (value < -limit || value >= limit)
		return std::nullopt;
	return (static_cast<std::uintptr_t>(value) &
		((std::uintptr_t{1} << baked_immediate_bits) - 1U)) <<
		baked_immediate_shift;
}

struct BakedAddress final {
	unsigned base{};
	unsigned index{};
	unsigned scale{};
	bool indexed{};
	std::uintptr_t displacement{};
	/* The access word without its displacement and immediate. */
	std::uintptr_t header{};

	/* Whether the access word holds the displacement: every access with a
	 * base register or an index, whose displacement is the instruction's
	 * 32 bits, and not a fused [address]. */
	[[nodiscard]] bool displaced() const noexcept
	{
		return indexed || base != baked::no_base_slot;
	}

	/* The access word, with `immediate` in its field. */
	[[nodiscard]] std::uintptr_t word(
		std::uintptr_t immediate = 0) const noexcept
	{
		return header | immediate |
			(displaced() ? static_cast<std::uint32_t>(displacement) : 0U);
	}
};

/* The plain 64-bit address shapes: no segment override, no narrow address
 * arithmetic, no vector index. A RIP-relative or absolute address is a
 * displacement with no base. An LEA, which cannot fault, has no use for the
 * access word's position and takes `access` false. */
[[nodiscard]] std::optional<BakedAddress> baked_address(
	const Gadget &gadget, std::uint64_t program_begin,
	const Operand &operand, bool access = true) noexcept
{
	if (operand.kind != OperandKind::memory)
		return std::nullopt;
	const auto &memory = operand.memory;
	if (memory.segment != Segment::none || memory.address_16 ||
	    memory.address_32 || memory.vector_index)
		return std::nullopt;
	BakedAddress address{};
	if (access) {
		const auto header = baked_access_header(gadget, program_begin);
		if (!header)
			return std::nullopt;
		address.header = *header;
	}
	if (memory.absolute_address) {
		address.base = baked::no_base_slot;
		address.displacement = memory.absolute;
	} else if (memory.rip_relative) {
		address.base = baked::no_base_slot;
		address.displacement = gadget.address + gadget.instruction.length +
			static_cast<std::int64_t>(memory.displacement);
	} else {
		if (memory.base == MemoryOperand::no_register)
			address.base = baked::no_base_slot;
		else if (memory.base < 16U)
			address.base = memory.base;
		else
			return std::nullopt;
		address.displacement = static_cast<std::uintptr_t>(
			static_cast<std::int64_t>(memory.displacement));
	}
	if (memory.index != MemoryOperand::no_register) {
		if (memory.index >= 16U || memory.absolute_address ||
		    memory.rip_relative)
			return std::nullopt;
		address.indexed = true;
		address.index = memory.index;
		address.scale = static_cast<unsigned>(
			std::countr_zero(static_cast<unsigned>(memory.scale)));
		if (address.scale >= baked::scale_slots)
			return std::nullopt;
	}
	return address;
}

/* The fused entry's operands: the access word, behind the 64-bit address of
 * an access with no base register. */
void append_baked_operands(StreamWords &stream,
	const BakedAddress &address, std::uintptr_t immediate = 0)
{
	if (!address.displaced())
		stream.push_back(address.displacement);
	stream.push_back(address.word(immediate));
}

/* The address entry of an indexed access, with the access word it hands to
 * the access entry behind it. */
void append_baked_index(StreamWords &stream,
	const BakedAddress &address, std::uintptr_t immediate = 0)
{
	stream.push_back(baked::uml_x86_64_baked_ea[
		(address.base * baked::gpr_slots + address.index) *
			baked::scale_slots + address.scale]);
	stream.push_back(address.word(immediate));
}

/* Emit a memory access: the fused entry for [base + displacement], or the
 * address entry followed by the split access entry. `slot` indexes the value
 * dimension of both tables. */
void append_baked_access(StreamWords &stream,
	const BakedAddress &address, const std::uintptr_t *fused,
	const std::uintptr_t *split, unsigned slot)
{
	if (!address.indexed) {
		stream.push_back(fused[slot * baked::base_slots + address.base]);
		append_baked_operands(stream, address);
		return;
	}
	append_baked_index(stream, address);
	stream.push_back(split[slot]);
}

/* The host flags a baked entry of this operation leaves. Narrow widths
 * qualify too: the bank compares the operands shifted to the top of a 32-bit
 * register, which sets NZCV exactly as the guest-width operation does. */
[[nodiscard]] HostFlags baked_flag_kind(Operation operation) noexcept
{
	switch (operation) {
	case Operation::add: return HostFlags::add;
	case Operation::subtract:
	case Operation::compare: return HostFlags::subtract;
	case Operation::bit_and:
	case Operation::test: return HostFlags::logical;
	default: return HostFlags::none;
	}
}

struct BakedArithTables final {
	const std::uintptr_t *rr{};
	const std::uintptr_t *ri{};
	const std::uintptr_t *rm{};
	const std::uintptr_t *xrm{};
	const std::uintptr_t *mr{};
	const std::uintptr_t *xmr{};
	const std::uintptr_t *mi{};
	const std::uintptr_t *xmi{};
};

[[nodiscard]] std::optional<BakedArithTables> baked_arith_tables(
	Operation operation, OperandWidth width) noexcept
{
#define UML_X86_BAKED_TABLES(op, bits, index) \
	return BakedArithTables{ \
		baked::uml_x86_64_baked_##op##_##bits##_rr, \
		baked::uml_x86_64_baked_##op##_##bits##_ri, \
		baked::uml_x86_64_baked_##op##_##bits##_rm, \
		baked::uml_x86_64_baked_##op##_##bits##_xrm, \
		baked::uml_x86_64_baked_##op##_##bits##_mr, \
		baked::uml_x86_64_baked_##op##_##bits##_xmr, \
		baked::uml_x86_64_baked_##op##_##bits##_mi, \
		baked::uml_x86_64_baked_##op##_xmi + (index)}
#define UML_X86_BAKED_OP(op) \
	switch (width) { \
	case OperandWidth::byte: UML_X86_BAKED_TABLES(op, 8, 0); \
	case OperandWidth::word: UML_X86_BAKED_TABLES(op, 16, 1); \
	case OperandWidth::dword: UML_X86_BAKED_TABLES(op, 32, 2); \
	case OperandWidth::qword: UML_X86_BAKED_TABLES(op, 64, 3); \
	default: return std::nullopt; \
	}
	switch (operation) {
	case Operation::add: UML_X86_BAKED_OP(add)
	case Operation::subtract: UML_X86_BAKED_OP(sub)
	case Operation::bit_and: UML_X86_BAKED_OP(and)
	case Operation::bit_or: UML_X86_BAKED_OP(or)
	case Operation::bit_xor: UML_X86_BAKED_OP(xor)
	case Operation::compare: UML_X86_BAKED_OP(cmp)
	case Operation::test: UML_X86_BAKED_OP(test)
	default: return std::nullopt;
	}
#undef UML_X86_BAKED_OP
#undef UML_X86_BAKED_TABLES
}

[[nodiscard]] std::optional<BakedArithTables> baked_move_tables(
	OperandWidth width) noexcept
{
#define UML_X86_BAKED_MOVE_TABLES(bits, index) \
	return BakedArithTables{ \
		baked::uml_x86_64_baked_move_##bits##_rr, \
		baked::uml_x86_64_baked_move_##bits##_ri, \
		baked::uml_x86_64_baked_move_##bits##_rm, \
		baked::uml_x86_64_baked_move_##bits##_xm, \
		baked::uml_x86_64_baked_move_##bits##_mr, \
		baked::uml_x86_64_baked_move_##bits##_xmr, \
		baked::uml_x86_64_baked_move_##bits##_mi, \
		baked::uml_x86_64_baked_move_xmi + (index)}
	switch (width) {
	case OperandWidth::byte: UML_X86_BAKED_MOVE_TABLES(8, 0);
	case OperandWidth::word: UML_X86_BAKED_MOVE_TABLES(16, 1);
	case OperandWidth::dword: UML_X86_BAKED_MOVE_TABLES(32, 2);
	case OperandWidth::qword: UML_X86_BAKED_MOVE_TABLES(64, 3);
	default: return std::nullopt;
	}
#undef UML_X86_BAKED_MOVE_TABLES
}

/* MOV and the two-operand arithmetic: reg,reg; reg,imm; reg,[mem]; [mem],reg;
 * [mem],imm. A locked form keeps the ordinary exclusive loop. */
[[nodiscard]] bool append_baked_binary(StreamWords &stream,
	const Gadget &gadget, std::uint64_t program_begin,
	const BakedArithTables &tables, FlagState &produced)
{
	const auto &instruction = gadget.instruction;
	if (instruction.locked)
		return false;
	const auto &destination = instruction.destination;
	const auto &source = instruction.source;
	const auto width = destination.width;
	const auto kind = baked_flag_kind(instruction.operation);
	/* A store's epilogue compares addresses, so only the non-writing
	 * memory-destination forms leave their flags for the next condition. */
	const bool writes_memory = instruction.operation != Operation::compare &&
		instruction.operation != Operation::test;
	if (destination.kind == OperandKind::reg) {
		produced = {kind, carry_of(kind)};
		const auto d = baked_register_slot(destination);
		if (!d)
			return false;
		if (source.kind == OperandKind::reg) {
			if (source.width != width)
				return false;
			const auto s = baked_register_slot(source);
			if (!s)
				return false;
			stream.push_back(tables.rr[*d * baked_value_slots(width) + *s]);
			return true;
		}
		if (source.kind == OperandKind::immediate) {
			stream.push_back(tables.ri[*d]);
			stream.push_back(source.immediate);
			return true;
		}
		if (source.kind != OperandKind::memory || source.width != width)
			return false;
		const auto address = baked_address(gadget, program_begin, source);
		if (!address)
			return false;
		append_baked_access(stream, *address, tables.rm, tables.xrm, *d);
		return true;
	}
	if (destination.kind != OperandKind::memory)
		return false;
	const auto address = baked_address(gadget, program_begin, destination);
	if (!address)
		return false;
	produced = writes_memory ? FlagState{} :
		FlagState{kind, carry_of(kind)};
	if (source.kind == OperandKind::reg) {
		if (source.width != width)
			return false;
		const auto s = baked_register_slot(source);
		if (!s)
			return false;
		append_baked_access(stream, *address, tables.mr, tables.xmr, *s);
		return true;
	}
	if (source.kind != OperandKind::immediate)
		return false;
	/* The immediate rides in the access word when it fits; an access with no
	 * base register has its address in the stream already and keeps the
	 * immediate in a word of its own. One that does not fit takes the
	 * ordinary bank. */
	const auto immediate = baked_packed_immediate(source.immediate, width);
	if (!address->displaced()) {
		stream.push_back(tables.mi[address->base]);
		append_baked_operands(stream, *address);
		stream.push_back(source.immediate);
		return true;
	}
	if (!immediate)
		return false;
	if (!address->indexed) {
		stream.push_back(tables.mi[address->base]);
		append_baked_operands(stream, *address, *immediate);
		return true;
	}
	append_baked_index(stream, *address, *immediate);
	stream.push_back(*tables.xmi);
	return true;
}

struct BakedExtendTables final {
	const std::uintptr_t *rr{};
	const std::uintptr_t *rm{};
	const std::uintptr_t *xm{};
};

[[nodiscard]] std::optional<BakedExtendTables> baked_extend_tables(
	bool sign, OperandWidth from, OperandWidth to) noexcept
{
#define UML_X86_BAKED_EXTEND_TABLES(name) \
	return BakedExtendTables{baked::uml_x86_64_baked_##name##_rr, \
		baked::uml_x86_64_baked_##name##_rm, baked::uml_x86_64_baked_##name##_xm}
	if (from == OperandWidth::byte && to == OperandWidth::dword)
		{ if (sign) UML_X86_BAKED_EXTEND_TABLES(sext_8_32); UML_X86_BAKED_EXTEND_TABLES(zext_8_32); }
	if (from == OperandWidth::byte && to == OperandWidth::qword)
		{ if (sign) UML_X86_BAKED_EXTEND_TABLES(sext_8_64); UML_X86_BAKED_EXTEND_TABLES(zext_8_64); }
	if (from == OperandWidth::word && to == OperandWidth::dword)
		{ if (sign) UML_X86_BAKED_EXTEND_TABLES(sext_16_32); UML_X86_BAKED_EXTEND_TABLES(zext_16_32); }
	if (from == OperandWidth::word && to == OperandWidth::qword)
		{ if (sign) UML_X86_BAKED_EXTEND_TABLES(sext_16_64); UML_X86_BAKED_EXTEND_TABLES(zext_16_64); }
	if (from == OperandWidth::dword && to == OperandWidth::qword && sign)
		UML_X86_BAKED_EXTEND_TABLES(sext_32_64);
	if (from == OperandWidth::byte && to == OperandWidth::word)
		return BakedExtendTables{sign ? baked::uml_x86_64_baked_sext_8_16_rr :
			baked::uml_x86_64_baked_zext_8_16_rr, nullptr, nullptr};
	return std::nullopt;
#undef UML_X86_BAKED_EXTEND_TABLES
}

[[nodiscard]] bool append_baked_extend(StreamWords &stream,
	const Gadget &gadget, std::uint64_t program_begin)
{
	const auto &instruction = gadget.instruction;
	const auto &destination = instruction.destination;
	const auto &source = instruction.source;
	const auto tables = baked_extend_tables(
		instruction.operation == Operation::move_sign_extend,
		source.width, destination.width);
	const auto d = baked_register_slot(destination);
	if (!tables || !d || destination.width == OperandWidth::byte)
		return false;
	if (source.kind == OperandKind::reg) {
		const auto s = baked_register_slot(source);
		if (!s)
			return false;
		stream.push_back(tables->rr[*d * baked_value_slots(source.width) + *s]);
		return true;
	}
	if (tables->rm == nullptr)
		return false;
	const auto address = baked_address(gadget, program_begin, source);
	if (!address)
		return false;
	append_baked_access(stream, *address, tables->rm, tables->xm, *d);
	return true;
}

/* One table per carry source and width; a source the bank does not cover
 * falls back to the entry that calls the materializer. */
using BakedUnaryCarryTables =
	std::array<std::array<const std::uintptr_t *, 4U>, 4U>;

#define UML_X86_BAKED_CARRY_TABLES(name) \
constexpr BakedUnaryCarryTables baked_unary_carry_tables_##name{{ \
	{baked::uml_x86_64_baked_##name##_settled_8_r, \
	 baked::uml_x86_64_baked_##name##_settled_16_r, \
	 baked::uml_x86_64_baked_##name##_settled_32_r, \
	 baked::uml_x86_64_baked_##name##_settled_64_r}, \
	{baked::uml_x86_64_baked_##name##_borrow_8_r, \
	 baked::uml_x86_64_baked_##name##_borrow_16_r, \
	 baked::uml_x86_64_baked_##name##_borrow_32_r, \
	 baked::uml_x86_64_baked_##name##_borrow_64_r}, \
	{baked::uml_x86_64_baked_##name##_carry_8_r, \
	 baked::uml_x86_64_baked_##name##_carry_16_r, \
	 baked::uml_x86_64_baked_##name##_carry_32_r, \
	 baked::uml_x86_64_baked_##name##_carry_64_r}, \
	{baked::uml_x86_64_baked_##name##_clear_8_r, \
	 baked::uml_x86_64_baked_##name##_clear_16_r, \
	 baked::uml_x86_64_baked_##name##_clear_32_r, \
	 baked::uml_x86_64_baked_##name##_clear_64_r}}};
UML_X86_BAKED_CARRY_TABLES(increment)
UML_X86_BAKED_CARRY_TABLES(decrement)
#undef UML_X86_BAKED_CARRY_TABLES

[[nodiscard]] const std::uintptr_t *baked_unary_carry_table(
	const BakedUnaryCarryTables &tables, CarrySource carry,
	OperandWidth width) noexcept
{
	std::size_t row = 0;
	switch (carry) {
	case CarrySource::settled: row = 0; break;
	case CarrySource::subtract: row = 1; break;
	case CarrySource::add: row = 2; break;
	case CarrySource::zero: row = 3; break;
	case CarrySource::unknown: return nullptr;
	}
	switch (width) {
	case OperandWidth::byte: return tables[row][0];
	case OperandWidth::word: return tables[row][1];
	case OperandWidth::dword: return tables[row][2];
	case OperandWidth::qword: return tables[row][3];
	default: return nullptr;
	}
}

[[nodiscard]] bool append_baked_unary(StreamWords &stream,
	const Gadget &gadget, const FlagState &state, FlagState &produced)
{
	const auto &instruction = gadget.instruction;
	/* A locked instruction has a memory destination, so it has no slot. */
	const auto d = baked_register_slot(instruction.destination);
	if (!d)
		return false;
	const bool wide = instruction.destination.width == OperandWidth::dword ||
		instruction.destination.width == OperandWidth::qword;
	const auto condition =
		wide && instruction.operation != Operation::bit_not ?
			HostFlags::no_carry : HostFlags::none;
	switch (instruction.operation) {
	case Operation::increment:
	case Operation::decrement:
		/* Both settle the carry, so the next one of them needs no work. */
		produced = {condition, CarrySource::settled};
		break;
	case Operation::negate:
		/* NEG is a subtraction from zero, so its host carry is a borrow. */
		produced = {condition,
			wide ? CarrySource::subtract : CarrySource::unknown};
		break;
	default:
		produced = {condition, CarrySource::unknown};
		break;
	}
	const std::uintptr_t *table{};
#define UML_X86_BAKED_UNARY(name) \
	switch (instruction.destination.width) { \
	case OperandWidth::byte: table = baked::uml_x86_64_baked_##name##_8_r; break; \
	case OperandWidth::word: table = baked::uml_x86_64_baked_##name##_16_r; break; \
	case OperandWidth::dword: table = baked::uml_x86_64_baked_##name##_32_r; break; \
	case OperandWidth::qword: table = baked::uml_x86_64_baked_##name##_64_r; break; \
	default: return false; \
	} \
	break;
#define UML_X86_BAKED_CARRY_UNARY(name) \
	table = baked_unary_carry_table( \
		baked_unary_carry_tables_##name, state.carry, \
		instruction.destination.width); \
	if (table == nullptr) UML_X86_BAKED_UNARY(name) \
	break;
	switch (instruction.operation) {
	case Operation::increment: UML_X86_BAKED_CARRY_UNARY(increment)
	case Operation::decrement: UML_X86_BAKED_CARRY_UNARY(decrement)
	case Operation::negate: UML_X86_BAKED_UNARY(negate)
	case Operation::bit_not: UML_X86_BAKED_UNARY(not)
	default: return false;
	}
#undef UML_X86_BAKED_CARRY_UNARY
#undef UML_X86_BAKED_UNARY
	stream.push_back(table[*d]);
	return true;
}

[[nodiscard]] bool append_baked_lea(StreamWords &stream,
	const Gadget &gadget, std::uint64_t program_begin)
{
	const auto &instruction = gadget.instruction;
	const auto d = baked_register_slot(instruction.destination);
	const auto address = baked_address(gadget, program_begin,
		instruction.source, false);
	if (!d || !address)
		return false;
	const std::uintptr_t *fused{};
	const std::uintptr_t *split{};
	switch (instruction.destination.width) {
	case OperandWidth::dword:
		fused = baked::uml_x86_64_baked_lea_32;
		split = baked::uml_x86_64_baked_lea_32_x;
		break;
	case OperandWidth::qword:
		fused = baked::uml_x86_64_baked_lea_64;
		split = baked::uml_x86_64_baked_lea_64_x;
		break;
	default: return false;
	}
	/* It cannot fault, so its stream is the displacement alone, or for an
	 * indexed address the address entry's word with only that field set. */
	if (!address->indexed) {
		stream.push_back(fused[*d * baked::base_slots + address->base]);
		stream.push_back(address->displacement);
		return true;
	}
	append_baked_index(stream, *address);
	stream.push_back(split[*d]);
	return true;
}

/* PUSH and POP of a register carry no stream word: their entry is all of
 * their stream. A fault or an overlap leaves with the stream pointer, and the
 * runtime finds the instruction from it, which only a block's own stream
 * allows -- a stream built from part of a block, for one run, takes the
 * ordinary bank, whose words carry the instruction's position. */
[[nodiscard]] bool append_baked_stack(StreamWords &stream,
	const Gadget &gadget)
{
	const auto &instruction = gadget.instruction;
	const bool push = instruction.operation == Operation::push;
	const auto &operand = push ? instruction.source : instruction.destination;
	if (!stream.block() || operand.width != OperandWidth::qword)
		return false;
	const auto slot = baked_register_slot(operand);
	if (!slot)
		return false;
	stream.push_back((push ? baked::uml_x86_64_baked_push_64_r :
		baked::uml_x86_64_baked_pop_64_r)[*slot]);
	return true;
}

/* The bank's entry point: true when the instruction was emitted as baked
 * entries, false to take the ordinary family. Only 64-bit mode is enumerated;
 * the stack forms in particular assume a 64-bit stack pointer. */
[[nodiscard]] bool append_baked(StreamWords &stream,
	const Gadget &gadget, ExecutionMode mode, std::uint64_t program_begin,
	const FlagState &state, FlagState &produced)
{
	produced = {};
	if (!baked_bank_enabled() || mode != ExecutionMode::long64)
		return false;
	const auto &instruction = gadget.instruction;
	switch (instruction.operation) {
	case Operation::move: {
		const auto tables = baked_move_tables(instruction.destination.width);
		return tables && append_baked_binary(stream, gadget,
			program_begin, *tables, produced);
	}
	case Operation::add:
	case Operation::subtract:
	case Operation::bit_and:
	case Operation::bit_or:
	case Operation::bit_xor:
	case Operation::compare:
	case Operation::test: {
		const auto tables = baked_arith_tables(
			instruction.operation, instruction.destination.width);
		return tables && append_baked_binary(stream, gadget,
			program_begin, *tables, produced);
	}
	case Operation::move_zero_extend:
	case Operation::move_sign_extend:
		return append_baked_extend(stream, gadget, program_begin);
	case Operation::increment:
	case Operation::decrement:
	case Operation::negate:
	case Operation::bit_not:
		return append_baked_unary(stream, gadget, state, produced);
	case Operation::lea:
		return append_baked_lea(stream, gadget, program_begin);
	case Operation::push:
	case Operation::pop:
		return append_baked_stack(stream, gadget);
	default:
		return false;
	}
}

/* A flag-neutral baked transfer hands the producer's host flags to the next
 * consumer, and the bank's miss tails save NZCV around the calls they make.
 * Nothing saves the vector result registers, which are call-clobbered, so a
 * vector result may only cross a transfer that cannot call out at all: one
 * that never accesses guest memory. LEA's memory operand is an address
 * computation, never an access; PUSH and POP always reach the stack. */
[[nodiscard]] bool baked_transfer_keeps_vector_results(
	const Instruction &instruction) noexcept
{
	if (instruction.operation == Operation::lea)
		return true;
	if (instruction.operation == Operation::push ||
		instruction.operation == Operation::pop)
		return false;
	return instruction.destination.kind != OperandKind::memory &&
		instruction.source.kind != OperandKind::memory;
}

/* `state` is what the previous gadget left for a fused condition and for a
 * carry-preserving consumer; `produced` receives what this one leaves.
 * `has_following` says whether more of this block is translated behind this
 * gadget, which is what lets a conditional branch inline its fall-through. */
[[nodiscard]] bool append_native(
	StreamWords &stream, const Gadget &gadget,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links, const FlagState &state,
	bool has_following, FlagState &produced)
{
	/* The decoder admits LOCK only on memory-destination read-modify-write
	 * operations (lock_allowed), and each of those has an atomic gadget. */
	const auto &instruction = gadget.instruction;
	/* The words that locate an instruction reach 1024 bytes into its block.
	 * A block of the default 64 instructions ends short of that; a longer
	 * one ends before the first instruction beyond, as at any instruction
	 * the bank has no form for. */
	if (!stream.position(gadget))
		return false;
	/* An access through FS or GS in compatibility mode that the general
	 * address tail does not check, and any through CS, wait for the
	 * runtime's exact check. */
	if (mode == ExecutionMode::compat32 && segment_guarded(instruction)) {
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_segment_guard));
		stream.push_back(stream.locator(gadget));
	}
	if (append_baked(stream, gadget, mode, program_begin, state, produced)) {
		/* A baked entry that executes no flag-setting host instruction
		 * leaves the producer's NZCV for a later consumer. The bank's
		 * transfers are all such entries: their translation tests the TLB
		 * tag with an exclusive-or, their store epilogue orders addresses by
		 * the sign of a difference, and their miss tails save NZCV around
		 * the calls they make. */
		const bool transparent =
			instruction.operation == Operation::move ||
			instruction.operation == Operation::move_zero_extend ||
			instruction.operation == Operation::move_sign_extend ||
			instruction.operation == Operation::lea ||
			instruction.operation == Operation::push ||
			instruction.operation == Operation::pop;
		if (transparent && produced.condition == HostFlags::none &&
			produced.carry == CarrySource::unknown) {
			produced = state;
			if (!baked_transfer_keeps_vector_results(instruction))
				produced.vector = {};
		}
		return true;
	}
	produced = {host_flags_of(instruction), ordinary_carry_of(instruction)};
	switch (instruction.operation) {
	case Operation::nop:
		stream.push_back(entry_address(native::uml_x86_64_gadget_nop));
		return true;
	case Operation::legacy_adjust: {
		NativeEntry entry{};
		switch (instruction.vector_opcode) {
		case 0x27: entry = native::uml_x86_64_gadget_daa; break;
		case 0x2f: entry = native::uml_x86_64_gadget_das; break;
		case 0x37: entry = native::uml_x86_64_gadget_aaa; break;
		case 0x3f: entry = native::uml_x86_64_gadget_aas; break;
		case 0xd4: entry = native::uml_x86_64_gadget_aam; break;
		case 0xd5: entry = native::uml_x86_64_gadget_aad; break;
		case 0xd6: entry = native::uml_x86_64_gadget_salc; break;
		default: return false;
		}
		stream.push_back(entry_address(entry));
		if (instruction.vector_opcode == 0xd4 ||
			instruction.vector_opcode == 0xd5) {
			stream.push_back(instruction.auxiliary.immediate);
			stream.push_back(gadget.address);
		}
		return true;
	}
	case Operation::interrupt_overflow:
		stream.push_back(entry_address(instruction.vector_opcode == 0xcd ?
			native::uml_x86_64_gadget_int4 :
			native::uml_x86_64_gadget_into));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	case Operation::move: {
		if (instruction.destination.kind == OperandKind::memory ||
			instruction.source.kind == OperandKind::memory)
			return append_memory_move(stream, gadget);
		const bool immediate = instruction.source.kind ==
			OperandKind::immediate;
		return append_register_binary(stream, gadget,
			select_move(instruction.destination.width, immediate));
	}
	case Operation::move_zero_extend:
	case Operation::move_sign_extend:
		if (instruction.source.kind == OperandKind::memory)
			return append_memory_extension(stream, gadget);
		return append_register_pair(stream,
			extension_entry(
				instruction.operation == Operation::move_sign_extend,
				instruction.source.width, instruction.destination.width),
			instruction.destination, instruction.source);
	case Operation::bound: {
		const auto index = register_offset(instruction.destination);
		const auto encoded = index ? encode_memory(
			gadget, instruction.source, *index) : std::nullopt;
		if (!encoded)
			return false;
		switch (instruction.destination.width) {
		case OperandWidth::word:
			stream.push_back(entry_address(native::uml_x86_64_gadget_bound_16));
			break;
		case OperandWidth::dword:
			stream.push_back(entry_address(native::uml_x86_64_gadget_bound_32));
			break;
		default:
			return false;
		}
		stream.push_back(encoded->descriptor | stream.locator(gadget));
		stream.push_back(encoded->displacement);
		return true;
	}
	case Operation::adjust_rpl:
		if (instruction.destination.kind == OperandKind::memory) {
			const auto source = register_offset(instruction.source);
			const auto encoded = source ? encode_memory(
				gadget, instruction.destination, *source) : std::nullopt;
			if (!encoded)
				return false;
			append_memory_words(stream, entry_address(
				native::uml_x86_64_gadget_arpl_16_mr),
				*encoded, gadget);
			return true;
		}
		return append_register_pair(stream,
			entry_address(native::uml_x86_64_gadget_arpl_16_rr),
			instruction.destination, instruction.source);
	case Operation::lea: {
		const auto destination = register_offset(instruction.destination);
		const auto encoded = destination ? encode_memory(
			gadget, instruction.source, *destination, false) : std::nullopt;
		if (!encoded)
			return false;
		std::uintptr_t entry{};
		switch (instruction.destination.width) {
		case OperandWidth::word:
			entry = entry_address(native::uml_x86_64_gadget_lea_16);
			break;
		case OperandWidth::dword:
			entry = entry_address(native::uml_x86_64_gadget_lea_32);
			break;
		case OperandWidth::qword:
			entry = entry_address(native::uml_x86_64_gadget_lea_64);
			break;
		default: return false;
		}
		append_memory_words(stream, entry, *encoded, gadget);
		return true;
	}
	case Operation::exchange: {
		if (instruction.destination.kind == OperandKind::memory) {
			const auto source = register_offset(instruction.source);
			const auto encoded = source ? encode_memory(
				gadget, instruction.destination, *source) : std::nullopt;
			std::uintptr_t entry{};
			switch (instruction.destination.width) {
			case OperandWidth::byte:
				entry = entry_address(native::uml_x86_64_gadget_exchange_8_mr);
				break;
			case OperandWidth::word:
				entry = entry_address(native::uml_x86_64_gadget_exchange_16_mr);
				break;
			case OperandWidth::dword:
				entry = entry_address(native::uml_x86_64_gadget_exchange_32_mr);
				break;
			case OperandWidth::qword:
				entry = entry_address(native::uml_x86_64_gadget_exchange_64_mr);
				break;
			default: return false;
			}
			if (!encoded)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		return append_register_pair(stream,
			select_exchange(instruction.destination.width),
			instruction.destination, instruction.source);
	}
	case Operation::add:
	case Operation::subtract:
	case Operation::bit_and:
	case Operation::bit_or:
	case Operation::bit_xor:
	case Operation::compare:
	case Operation::test: {
		if (instruction.destination.kind == OperandKind::memory ||
			instruction.source.kind == OperandKind::memory)
			return append_memory_arithmetic(stream, gadget);
		const bool immediate = instruction.source.kind ==
			OperandKind::immediate;
		return append_register_binary(stream, gadget,
			arithmetic_entry(instruction.operation,
				instruction.destination.width, immediate));
	}
	case Operation::add_carry:
	case Operation::subtract_borrow: {
		if (instruction.destination.kind == OperandKind::memory ||
			instruction.source.kind == OperandKind::memory)
			return append_memory_arithmetic(stream, gadget);
		const bool immediate = instruction.source.kind == OperandKind::immediate;
		return append_register_binary(stream, gadget,
			instruction.operation == Operation::add_carry ?
				select_adc(instruction.destination.width, immediate) :
				select_sbb(instruction.destination.width, immediate));
	}
	case Operation::increment:
	case Operation::decrement:
	case Operation::negate:
	case Operation::bit_not: {
		if (instruction.destination.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.destination, 0);
			std::uintptr_t entry{};
			const auto unary_width = instruction.destination.width;
			const bool locked = instruction.locked;
			switch (instruction.operation) {
			case Operation::increment:
				entry = locked ?
					select_memory_increment_locked(unary_width) :
					select_memory_increment(unary_width);
				break;
			case Operation::decrement:
				entry = locked ?
					select_memory_decrement_locked(unary_width) :
					select_memory_decrement(unary_width);
				break;
			case Operation::negate:
				entry = locked ? select_memory_negate_locked(unary_width) :
					select_memory_negate(unary_width);
				break;
			case Operation::bit_not:
				entry = locked ? select_memory_not_locked(unary_width) :
					select_memory_not(unary_width);
				break;
			default: break;
			}
			if (!encoded || entry == 0)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		std::uintptr_t entry{};
		switch (instruction.operation) {
		case Operation::increment:
			entry = select_increment(instruction.destination.width);
			break;
		case Operation::decrement:
			entry = select_decrement(instruction.destination.width);
			break;
		case Operation::negate:
			entry = select_negate(instruction.destination.width);
			break;
		case Operation::bit_not:
			entry = select_not(instruction.destination.width);
			break;
		default: break;
		}
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(*destination);
		return true;
	}
	case Operation::shift_left:
	case Operation::shift_right:
	case Operation::shift_arithmetic_right:
	case Operation::rotate_left:
	case Operation::rotate_right:
	case Operation::rotate_carry_left:
	case Operation::rotate_carry_right:
		return append_shift(stream, gadget);
	case Operation::double_shift_left:
	case Operation::double_shift_right:
		return append_double_shift(stream, gadget);
	case Operation::multiply:
	case Operation::multiply_signed:
		return append_multiply(stream, gadget);
	case Operation::divide:
	case Operation::divide_signed:
		return append_divide(stream, gadget);
	case Operation::bit_test:
	case Operation::bit_set:
	case Operation::bit_reset:
	case Operation::bit_complement:
		return append_bit_operation(stream, gadget);
	case Operation::bit_scan_forward:
	case Operation::bit_scan_reverse:
	case Operation::count_trailing_zeros:
	case Operation::count_leading_zeros:
	case Operation::population_count:
		return append_bit_count(stream, gadget);
	case Operation::compare_exchange:
	case Operation::exchange_add:
		return append_atomic_binary(stream, gadget);
	case Operation::compare_exchange_pair:
		return append_compare_exchange_pair(stream, gadget);
	case Operation::move_byte_swap:
		return append_movbe(stream, gadget);
	case Operation::crc32:
		return append_crc32(stream, gadget);
	case Operation::adx:
		return append_adx(stream, gadget);
	case Operation::scalar_bmi:
		return append_scalar_bmi(stream, gadget);
	case Operation::byte_swap: {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		const auto entry = instruction.destination.width == OperandWidth::qword ?
			entry_address(native::uml_x86_64_gadget_byte_swap_64) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_byte_swap_32) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(*destination);
		return true;
	}
	case Operation::sign_extend_accumulator:
	case Operation::sign_extend_data: {
		const bool accumulator = instruction.operation ==
			Operation::sign_extend_accumulator;
		std::uintptr_t entry{};
		switch (instruction.destination.width) {
		case OperandWidth::word:
			entry = entry_address(accumulator ?
				native::uml_x86_64_gadget_sign_extend_accumulator_16 :
				native::uml_x86_64_gadget_sign_extend_data_16);
			break;
		case OperandWidth::dword:
			entry = entry_address(accumulator ?
				native::uml_x86_64_gadget_sign_extend_accumulator_32 :
				native::uml_x86_64_gadget_sign_extend_data_32);
			break;
		case OperandWidth::qword:
			entry = entry_address(accumulator ?
				native::uml_x86_64_gadget_sign_extend_accumulator_64 :
				native::uml_x86_64_gadget_sign_extend_data_64);
			break;
		default: return false;
		}
		stream.push_back(entry);
		return true;
	}
	case Operation::carry_clear:
		stream.push_back(entry_address(native::uml_x86_64_gadget_carry_clear));
		return true;
	case Operation::carry_set:
		stream.push_back(entry_address(native::uml_x86_64_gadget_carry_set));
		return true;
	case Operation::carry_complement:
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_carry_complement));
		return true;
	case Operation::direction_clear:
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_direction_clear));
		return true;
	case Operation::direction_set:
		stream.push_back(entry_address(native::uml_x86_64_gadget_direction_set));
		return true;
	case Operation::lahf:
		stream.push_back(entry_address(native::uml_x86_64_gadget_lahf));
		return true;
	case Operation::sahf:
		stream.push_back(entry_address(native::uml_x86_64_gadget_sahf));
		return true;
	case Operation::segment_move:
	case Operation::segment_stack:
	case Operation::load_far_pointer:
	case Operation::far_call:
	case Operation::far_jump:
	case Operation::far_ret:
	case Operation::load_access_rights:
	case Operation::verify_segment:
	case Operation::store_segment_table:
		if (segment_instruction(instruction)) {
			stream.push_back(entry_address(
				native::uml_x86_64_gadget_segment_instruction));
			stream.push_back(gadget.address);
			return true;
		}
		return instruction.operation == Operation::segment_move ?
			append_segment_move(stream, gadget) :
			append_segment_stack(stream, gadget);
	case Operation::set_condition: {
		if (instruction.destination.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.destination, 0);
			const auto entry = set_condition_memory_entry(instruction.condition);
			if (!encoded || entry == 0)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		const auto destination = register_offset(instruction.destination);
		const auto entry = fused_condition(instruction.condition, state.condition) ?
			set_condition_fused_entry(fused_consumer_condition(
				instruction.condition, state.condition)) :
			set_condition_entry(instruction.condition);
		if (!destination || entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(*destination);
		return true;
	}
	case Operation::conditional_move:
		if (instruction.source.kind == OperandKind::memory) {
			const auto destination = register_offset(instruction.destination);
			const auto encoded = destination ? encode_memory(
				gadget, instruction.source, *destination) : std::nullopt;
			const auto entry = conditional_move_memory_entry(
				instruction.condition, instruction.destination.width);
			if (!encoded || entry == 0)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		return append_register_pair(stream,
			fused_condition(instruction.condition, state.condition) ?
				conditional_move_fused_entry(fused_consumer_condition(
					instruction.condition, state.condition),
					instruction.destination.width) :
				conditional_move_entry(
					instruction.condition, instruction.destination.width),
			instruction.destination, instruction.source);
	case Operation::jump: {
		const auto target = relative_target(gadget, mode);
		if (links.resolve != nullptr) {
			const auto *link = links.resolve(links.context, target, mode);
			if (link != nullptr) {
				stream.push_back(entry_address(
					native::uml_x86_64_gadget_jump_linked));
				stream.push_back(reinterpret_cast<std::uintptr_t>(link));
				return true;
			}
		}
		stream.push_back(entry_address(native::uml_x86_64_gadget_jump));
		stream.push_back(target);
		return true;
	}
	case Operation::jump_conditional: {
		const auto target = relative_target(gadget, mode);
		const auto next = next_instruction_pointer(gadget, mode);
		const auto *taken = links.resolve == nullptr ? nullptr :
			links.resolve(links.context, target, mode);
		/* The fall-through is already translated behind this branch, so it
		 * needs neither a link record nor an operand. A build without a link
		 * resolver -- a budget-limited replay of a cached block -- keeps the
		 * terminal form and simply leaves the translated tail unreachable. */
		if (has_following && taken != nullptr) {
			const auto inlined =
				fused_condition(instruction.condition, state.condition) ?
					jump_condition_inline_fused_entry(
						fused_consumer_condition(
							instruction.condition, state.condition)) :
					jump_condition_inline_entry(instruction.condition);
			if (inlined != 0) {
				stream.push_back(inlined);
				stream.push_back(
					reinterpret_cast<std::uintptr_t>(taken));
				return true;
			}
		}
		const auto *fallthrough = links.resolve == nullptr ? nullptr :
			links.resolve(links.context, next, mode);
		const bool linked = taken != nullptr && fallthrough != nullptr;
		const auto entry = fused_condition(instruction.condition, state.condition) ?
			jump_condition_fused_entry(fused_consumer_condition(
				instruction.condition, state.condition), linked) :
			jump_condition_entry(instruction.condition, linked);
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(linked ? reinterpret_cast<std::uintptr_t>(taken) :
			target);
		stream.push_back(linked ?
			reinterpret_cast<std::uintptr_t>(fallthrough) : next);
		return true;
	}
	case Operation::loop: {
		NativeEntry entry{};
#define UML_X86_SELECT_LOOP(opcode) \
		entry = instruction.vector_control == 2 ? \
			native::uml_x86_64_gadget_loop_##opcode##_16 : \
			instruction.vector_control == 1 ? \
				native::uml_x86_64_gadget_loop_##opcode##_32 : \
				native::uml_x86_64_gadget_loop_##opcode##_64
		switch (instruction.vector_opcode) {
		case 0xe0: UML_X86_SELECT_LOOP(e0); break;
		case 0xe1: UML_X86_SELECT_LOOP(e1); break;
		case 0xe2: UML_X86_SELECT_LOOP(e2); break;
		case 0xe3: UML_X86_SELECT_LOOP(e3); break;
		default: return false;
		}
#undef UML_X86_SELECT_LOOP
		const auto next = next_instruction_pointer(gadget, mode);
		stream.push_back(entry_address(entry));
		stream.push_back(relative_target(gadget, mode));
		stream.push_back(next);
		return true;
	}
	case Operation::jump_indirect: {
		auto *site = links.resolve_indirect == nullptr ? nullptr :
			links.resolve_indirect(links.context, gadget.address, mode);
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.source, 0);
			if (!encoded)
				return false;
			append_memory_words(stream, entry_address(
				mode == ExecutionMode::compat32 ?
					(site ? native::uml_x86_64_gadget_jump_indirect_32_m_cached :
						native::uml_x86_64_gadget_jump_indirect_32_m) :
					(site ? native::uml_x86_64_gadget_jump_indirect_m_cached :
						native::uml_x86_64_gadget_jump_indirect_m)),
						*encoded, gadget);
			if (site)
				stream.push_back(reinterpret_cast<std::uintptr_t>(site));
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry_address(
			mode == ExecutionMode::compat32 ?
				(site ? native::uml_x86_64_gadget_jump_indirect_32_r_cached :
					native::uml_x86_64_gadget_jump_indirect_32_r) :
				(site ? native::uml_x86_64_gadget_jump_indirect_r_cached :
					native::uml_x86_64_gadget_jump_indirect_r)));
		stream.push_back(*source);
		if (site)
			stream.push_back(reinterpret_cast<std::uintptr_t>(site));
		return true;
	}
	case Operation::push: {
		const bool immediate = instruction.source.kind ==
			OperandKind::immediate;
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.source, 0);
			const auto entry = instruction.source.width == OperandWidth::word ?
				entry_address(native::uml_x86_64_gadget_push_16_m) :
				instruction.source.width == OperandWidth::dword ?
				entry_address(native::uml_x86_64_gadget_push_32_m) :
				instruction.source.width == OperandWidth::qword ?
				entry_address(native::uml_x86_64_gadget_push_64_m) : 0;
			if (!encoded || entry == 0)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		const auto source = immediate ? std::optional<std::uint16_t>{0} :
			register_offset(instruction.source);
		if (!source)
			return false;
		std::uintptr_t entry{};
		if (instruction.source.width == OperandWidth::word)
			entry = entry_address(immediate ?
				native::uml_x86_64_gadget_push_16_i :
				native::uml_x86_64_gadget_push_16_r);
		else if (instruction.source.width == OperandWidth::dword)
			entry = entry_address(immediate ?
				native::uml_x86_64_gadget_push_32_i :
				native::uml_x86_64_gadget_push_32_r);
		else if (instruction.source.width == OperandWidth::qword)
			entry = entry_address(immediate ?
				native::uml_x86_64_gadget_push_64_i :
				native::uml_x86_64_gadget_push_64_r);
		else
			return false;
		const auto descriptor = static_cast<std::uintptr_t>(*source) |
			stream.locator(gadget);
		stream.push_back(entry);
		stream.push_back(descriptor);
		if (immediate)
			stream.push_back(instruction.source.immediate);
		return true;
	}
	case Operation::pop: {
		if (instruction.destination.kind == OperandKind::memory) {
			const auto encoded = encode_memory(
				gadget, instruction.destination, 0);
			const auto entry = instruction.destination.width == OperandWidth::word ?
				entry_address(native::uml_x86_64_gadget_pop_16_m) :
				instruction.destination.width == OperandWidth::dword ?
				entry_address(native::uml_x86_64_gadget_pop_32_m) :
				instruction.destination.width == OperandWidth::qword ?
				entry_address(native::uml_x86_64_gadget_pop_64_m) : 0;
			if (!encoded || entry == 0)
				return false;
			append_memory_words(stream, entry, *encoded, gadget);
			return true;
		}
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		std::uintptr_t entry{};
		if (instruction.destination.width == OperandWidth::word)
			entry = entry_address(native::uml_x86_64_gadget_pop_16_r);
		else if (instruction.destination.width == OperandWidth::dword)
			entry = entry_address(native::uml_x86_64_gadget_pop_32_r);
		else if (instruction.destination.width == OperandWidth::qword)
			entry = entry_address(native::uml_x86_64_gadget_pop_64_r);
		else
			return false;
		stream.push_back(entry);
		stream.push_back(static_cast<std::uintptr_t>(*destination) |
			stream.locator(gadget));
		return true;
	}
	case Operation::push_all: {
		const auto entry = instruction.destination.width == OperandWidth::word ?
			entry_address(native::uml_x86_64_gadget_push_all_16) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_push_all_32) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::pop_all: {
		const auto entry = instruction.destination.width == OperandWidth::word ?
			entry_address(native::uml_x86_64_gadget_pop_all_16) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_pop_all_32) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::push_flags: {
		const auto entry = instruction.source.width == OperandWidth::word ?
			entry_address(native::uml_x86_64_gadget_push_flags_16) :
			instruction.source.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_push_flags_32) :
			instruction.source.width == OperandWidth::qword ?
			entry_address(native::uml_x86_64_gadget_push_flags_64) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::pop_flags: {
		const auto entry = instruction.destination.width == OperandWidth::word ?
			entry_address(native::uml_x86_64_gadget_pop_flags_16) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_pop_flags_32) :
			instruction.destination.width == OperandWidth::qword ?
			entry_address(native::uml_x86_64_gadget_pop_flags_64) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(stream.locator(gadget));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	}
	case Operation::leave: {
		const auto entry = instruction.destination.width == OperandWidth::word ?
			entry_address(native::uml_x86_64_gadget_leave_16) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_leave_32) :
			instruction.destination.width == OperandWidth::qword ?
			entry_address(native::uml_x86_64_gadget_leave_64) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::enter:
		return append_enter(stream, gadget);
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan:
		return append_string(stream, gadget);
	case Operation::call: {
		const auto target = relative_target(gadget, mode);
		const auto *link = links.resolve == nullptr ? nullptr :
			links.resolve(links.context, target, mode);
		const bool linked = link != nullptr;
		const auto next = next_instruction_pointer(gadget, mode);
		/* A CALL of the five bytes the opcode and its displacement make has
		 * its address in the return address it pushes, five bytes before it.
		 * One with a prefix before that carries the word that locates it. */
		const bool bare = instruction.length == direct_call_length;
		const bool compat = mode == ExecutionMode::compat32;
		stream.push_back(entry_address(compat ?
			(linked ? (bare ? native::uml_x86_64_gadget_call_32_linked :
					native::uml_x86_64_gadget_call_32_positioned_linked) :
				(bare ? native::uml_x86_64_gadget_call_32 :
					native::uml_x86_64_gadget_call_32_positioned)) :
			(linked ? (bare ? native::uml_x86_64_gadget_call_linked :
					native::uml_x86_64_gadget_call_positioned_linked) :
				(bare ? native::uml_x86_64_gadget_call :
					native::uml_x86_64_gadget_call_positioned))));
		stream.push_back(linked ? reinterpret_cast<std::uintptr_t>(link) :
			target);
		stream.push_back(next);
		if (!bare)
			stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::call_indirect: {
		auto *site = links.resolve_indirect == nullptr ? nullptr :
			links.resolve_indirect(links.context, gadget.address, mode);
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.source, 0);
			if (!encoded)
				return false;
			const auto next = next_instruction_pointer(gadget, mode);
			append_memory_words(stream, entry_address(
				mode == ExecutionMode::compat32 ?
					(site ? native::uml_x86_64_gadget_call_indirect_32_m_cached :
						native::uml_x86_64_gadget_call_indirect_32_m) :
					(site ? native::uml_x86_64_gadget_call_indirect_m_cached :
						native::uml_x86_64_gadget_call_indirect_m)),
						*encoded, gadget);
			stream.push_back(next);
			if (site)
				stream.push_back(reinterpret_cast<std::uintptr_t>(site));
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		const auto next = next_instruction_pointer(gadget, mode);
		stream.push_back(entry_address(
			mode == ExecutionMode::compat32 ?
				(site ? native::uml_x86_64_gadget_call_indirect_32_r_cached :
					native::uml_x86_64_gadget_call_indirect_32_r) :
				(site ? native::uml_x86_64_gadget_call_indirect_r_cached :
					native::uml_x86_64_gadget_call_indirect_r)));
		stream.push_back(*source | stream.locator(gadget));
		stream.push_back(next);
		if (site)
			stream.push_back(reinterpret_cast<std::uintptr_t>(site));
		return true;
	}
	case Operation::ret: {
		const bool linked = links.resolve != nullptr;
		stream.push_back(entry_address(mode == ExecutionMode::compat32 ?
			(linked ? native::uml_x86_64_gadget_return_32_cached :
				native::uml_x86_64_gadget_return_32) :
			(linked ? native::uml_x86_64_gadget_return_cached :
				native::uml_x86_64_gadget_return)));
		stream.push_back((instruction.source.immediate & 0xffffU) |
			stream.locator(gadget));
		return true;
	}
	case Operation::interrupt_ret: {
		const auto entry = interrupt_return_entry(
			instruction.destination.width, mode);
		if (entry == nullptr)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(gadget.address);
		return true;
	}
	case Operation::system_call:
		stream.push_back(entry_address(instruction.vector_opcode == 0xcd ?
			native::uml_x86_64_gadget_int80 :
			native::uml_x86_64_gadget_syscall));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	case Operation::breakpoint:
		stream.push_back(entry_address(native::uml_x86_64_gadget_breakpoint));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	case Operation::debug_trap:
		stream.push_back(entry_address(native::uml_x86_64_gadget_icebp));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	case Operation::privileged:
		stream.push_back(entry_address(native::uml_x86_64_gadget_privileged));
		stream.push_back(stream.locator(gadget));
		return true;
	case Operation::cpuid:
		stream.push_back(entry_address(native::uml_x86_64_gadget_cpuid));
		stream.push_back(next_instruction_pointer(gadget, mode));
		return true;
	case Operation::read_extended_control:
		stream.push_back(entry_address(native::uml_x86_64_gadget_xgetbv));
		return true;
	case Operation::read_timestamp:
		stream.push_back(entry_address(
			instruction.auxiliary.immediate != 0 ?
				native::uml_x86_64_gadget_rdtscp :
				native::uml_x86_64_gadget_rdtsc));
		stream.push_back(stream.locator(gadget));
		return true;
	case Operation::read_processor_id: {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		const auto entry = instruction.destination.width == OperandWidth::qword ?
			entry_address(native::uml_x86_64_gadget_rdpid_64_r) :
			instruction.destination.width == OperandWidth::dword ?
			entry_address(native::uml_x86_64_gadget_rdpid_32_r) : 0;
		if (entry == 0)
			return false;
		stream.push_back(entry);
		stream.push_back(*destination);
		return true;
	}
	case Operation::xlat: {
		if (instruction.source.kind != OperandKind::memory)
			return false;
		const auto config =
			static_cast<std::uintptr_t>(instruction.source.memory.address_32) |
			(static_cast<std::uintptr_t>(instruction.source.memory.segment) << 1U) |
			(static_cast<std::uintptr_t>(
				instruction.source.memory.address_16) << 3U);
		stream.push_back(entry_address(native::uml_x86_64_gadget_xlat));
		stream.push_back(config | stream.locator(gadget));
		return true;
	}
	case Operation::random: {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		NativeEntry entry{};
		switch (instruction.destination.width) {
		case OperandWidth::word:
			entry = native::uml_x86_64_gadget_random_16;
			break;
		case OperandWidth::dword:
			entry = native::uml_x86_64_gadget_random_32;
			break;
		case OperandWidth::qword:
			entry = native::uml_x86_64_gadget_random_64;
			break;
		default: return false;
		}
		stream.push_back(entry_address(entry));
		stream.push_back(*destination);
		return true;
	}
	case Operation::read_fs_base:
	case Operation::read_gs_base:
	case Operation::write_fs_base:
	case Operation::write_gs_base: {
		const bool read = instruction.operation == Operation::read_fs_base ||
			instruction.operation == Operation::read_gs_base;
		const auto &operand = read ? instruction.destination : instruction.source;
		const auto offset = register_offset(operand);
		if (!offset)
			return false;
		const bool fs = instruction.operation == Operation::read_fs_base ||
			instruction.operation == Operation::write_fs_base;
		NativeEntry entry{};
		if (read && fs && operand.width == OperandWidth::dword)
			entry = native::uml_x86_64_gadget_read_fs_base_32_r;
		else if (read && fs && operand.width == OperandWidth::qword)
			entry = native::uml_x86_64_gadget_read_fs_base_64_r;
		else if (read && !fs && operand.width == OperandWidth::dword)
			entry = native::uml_x86_64_gadget_read_gs_base_32_r;
		else if (read && !fs && operand.width == OperandWidth::qword)
			entry = native::uml_x86_64_gadget_read_gs_base_64_r;
		else if (!read && fs && operand.width == OperandWidth::dword)
			entry = native::uml_x86_64_gadget_write_fs_base_32_r;
		else if (!read && fs && operand.width == OperandWidth::qword)
			entry = native::uml_x86_64_gadget_write_fs_base_64_r;
		else if (!read && !fs && operand.width == OperandWidth::dword)
			entry = native::uml_x86_64_gadget_write_gs_base_32_r;
		else if (!read && !fs && operand.width == OperandWidth::qword)
			entry = native::uml_x86_64_gadget_write_gs_base_64_r;
		if (entry == nullptr)
			return false;
		stream.push_back(entry_address(entry));
		stream.push_back(*offset);
		/* A 64-bit write raises #GP at this instruction when the base is
		 * not canonical. */
		if (!read && operand.width == OperandWidth::qword)
			stream.push_back(stream.locator(gadget));
		return true;
	}
	case Operation::memory_fence:
		stream.push_back(entry_address(native::uml_x86_64_gadget_memory_fence));
		return true;
	case Operation::cache_flush: {
		const auto encoded = encode_memory(gadget, instruction.destination, 0);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(
			native::uml_x86_64_gadget_cache_flush), *encoded, gadget);
		return true;
	}
	case Operation::store_machine_status: {
		if (instruction.destination.kind == OperandKind::reg) {
			const auto destination = register_offset(instruction.destination);
			if (!destination)
				return false;
			NativeEntry entry{};
			switch (instruction.destination.width) {
			case OperandWidth::word:
				entry = native::uml_x86_64_gadget_store_machine_status_16_r;
				break;
			case OperandWidth::dword:
				entry = native::uml_x86_64_gadget_store_machine_status_32_r;
				break;
			case OperandWidth::qword:
				entry = native::uml_x86_64_gadget_store_machine_status_64_r;
				break;
			default: return false;
			}
			stream.push_back(entry_address(entry));
			stream.push_back(*destination);
			return true;
		}
		const auto encoded = encode_memory(gadget, instruction.destination, 0);
		if (!encoded || instruction.destination.width != OperandWidth::word)
			return false;
		append_memory_words(stream, entry_address(
			native::uml_x86_64_gadget_store_machine_status_16_m),
			*encoded, gadget);
		return true;
	}
	case Operation::store_gdtr: {
		const auto encoded = encode_memory(gadget, instruction.destination, 0);
		if (!encoded)
			return false;
		const bool compat = mode == ExecutionMode::compat32;
		const auto entry = instruction.vector_group == 1 ?
			(compat ? native::uml_x86_64_gadget_store_idtr_32 :
				native::uml_x86_64_gadget_store_idtr) :
			(compat ? native::uml_x86_64_gadget_store_gdtr_32 :
				native::uml_x86_64_gadget_store_gdtr);
		append_memory_words(stream, entry_address(entry), *encoded, gadget);
		return true;
	}
	case Operation::load_segment_limit: {
		const auto destination = register_offset(instruction.destination);
		if (!destination)
			return false;
		const auto width = instruction.destination.width;
		const std::uint16_t target = static_cast<std::uint16_t>(*destination |
			((width == OperandWidth::word ? 1U :
				width == OperandWidth::dword ? 2U : 3U) << 8U));
		if (instruction.source.kind == OperandKind::memory) {
			const auto encoded = encode_memory(gadget, instruction.source,
				target);
			if (!encoded)
				return false;
			append_memory_words(stream, entry_address(
				native::uml_x86_64_gadget_load_segment_limit_m),
				*encoded, gadget);
			return true;
		}
		const auto source = register_offset(instruction.source);
		if (!source)
			return false;
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_load_segment_limit_r));
		stream.push_back(*source);
		stream.push_back(target);
		return true;
	}
	case Operation::mxcsr: {
		const auto encoded = encode_memory(gadget, instruction.destination, 0);
		if (!encoded)
			return false;
		append_memory_words(stream, entry_address(
			instruction.vector_group == 2 ?
			native::uml_x86_64_gadget_mxcsr_load :
			native::uml_x86_64_gadget_mxcsr_store),
			*encoded, gadget);
		return true;
	}
	case Operation::extended_state:
		return append_extended_state(stream, gadget);
	case Operation::x87:
		return append_x87(stream, gadget);
	case Operation::vector_move:
		return append_vector_move(stream, gadget);
	case Operation::vector_move_scalar:
		return append_vector_move_scalar(stream, gadget);
	case Operation::vector_to_gpr:
		return append_vector_to_gpr(stream, gadget);
	case Operation::vector_insert:
		return append_vector_insert(stream, gadget);
	case Operation::vector_test:
		return append_vector_test(stream, gadget);
	case Operation::vector_compare_flags:
		return append_vector_compare_flags(stream, gadget);
	case Operation::vector_gpr_move:
		return append_vector_gpr_move(stream, gadget);
	case Operation::vector_mask_store:
		return append_vector_mask_store(stream, gadget);
	case Operation::vector_lane_move:
		return append_vector_lane_move(stream, gadget);
	case Operation::vector_alu:
		if (append_vector_bitwise(stream, gadget))
			return true;
		if (append_vector_shuffle(stream, gadget))
			return true;
		return append_vector_float(stream, gadget);
	case Operation::vector_convert:
		return append_vector_convert(stream, gadget);
	case Operation::vector_scalar_convert:
		return append_vector_scalar_convert(stream, gadget);
	case Operation::vector_fma:
		return append_vector_fma(stream, gadget);
	case Operation::vector_dot:
		return append_vector_dot(stream, gadget);
	case Operation::vector_aes:
		return append_vector_aes(stream, gadget);
	case Operation::vector_sha:
		return append_vector_sha(stream, gadget);
	case Operation::vector_integer:
		if (!append_vector_integer(stream, gadget, state.vector))
			return false;
		note_vector_result(instruction, produced.vector,
			vector_integer_forwards(instruction));
		return true;
	case Operation::vector_integer_map2:
		return append_vector_integer_map2(stream, gadget);
	case Operation::vector_map2_misc:
		return append_vector_map2_misc(stream, gadget);
	case Operation::vector_gather:
		return append_vector_gather(stream, gadget);
	case Operation::vector_integer_map3:
		return append_vector_integer_map3(stream, gadget);
	case Operation::vector_zero:
		stream.push_back(entry_address(
			instruction.auxiliary.immediate != 0 ?
				native::uml_x86_64_gadget_vector_zero_all :
				native::uml_x86_64_gadget_vector_zero_upper));
		return true;
	default:
		return false;
	}
}

} // namespace

std::optional<std::size_t> append_native_stream(
	std::vector<std::uintptr_t> &stream, std::span<const Gadget> gadgets,
	std::uint64_t final_rip, ExecutionMode mode,
	std::uint64_t program_begin, ProgramLinkResolver links, StreamKind kind)
{
	stream.reserve(stream.size() + gadgets.size() * 3U + 2U);
	StreamWords words{stream, mode, program_begin, kind};
	FlagState state{};
	for (std::size_t index = 0; index < gadgets.size(); ++index) {
		FlagState produced{};
		if (!append_native(words, gadgets[index], mode, program_begin,
			links, state, index + 1U < gadgets.size(), produced))
			return index;
		state = produced;
	}
	/* Every gadget a terminal instruction selects leaves or follows a link
	 * on each of its paths, so a block that ended at a branch never reaches
	 * the end of its stream and carries no terminator. Only a block the
	 * fetch window cut short falls off its last instruction.
	 */
	if (!gadgets.empty() && gadgets.back().instruction.terminal())
		return std::nullopt;
	const auto next = normalize_instruction_pointer(mode, final_rip);
	const auto *link = !gadgets.empty() && links.resolve != nullptr ?
		links.resolve(links.context, next, mode) : nullptr;
	if (link != nullptr) {
		stream.push_back(entry_address(
			native::uml_x86_64_gadget_block_exit_linked));
		stream.push_back(reinterpret_cast<std::uintptr_t>(link));
		return std::nullopt;
	}
	stream.push_back(entry_address(native::uml_x86_64_gadget_block_exit));
	stream.push_back(next);
	return std::nullopt;
}

std::optional<std::size_t> instruction_at_word(
	std::span<const Gadget> gadgets, std::span<const std::uintptr_t> stream,
	std::size_t word, ExecutionMode mode, std::uint64_t program_begin,
	StreamKind kind)
{
	/* Only a branch's form depends on the links its build found, and the
	 * forms differ in their first word, so each instruction is built both
	 * ways and the one the stream holds says how far it reaches. */
	static BlockLink placeholder_link{};
	static IndirectLink placeholder_site{};
	const ProgramLinkResolver linked{
		.context = nullptr,
		.resolve = [](void *, std::uint64_t, ExecutionMode) noexcept ->
			const BlockLink * { return &placeholder_link; },
		.resolve_indirect = [](void *, std::uint64_t, ExecutionMode) noexcept ->
			IndirectLink * { return &placeholder_site; },
	};
	std::vector<std::uintptr_t> words;
	StreamWords builder{words, mode, program_begin, kind};
	FlagState state{};
	std::size_t position{};
	for (std::size_t index = 0; index < gadgets.size(); ++index) {
		FlagState produced{};
		bool found = false;
		for (const auto links : {ProgramLinkResolver{}, linked}) {
			words.clear();
			produced = {};
			if (!append_native(builder, gadgets[index], mode, program_begin,
				links, state, index + 1U < gadgets.size(), produced))
				return std::nullopt;
			if (!words.empty() && position + words.size() <= stream.size() &&
				words.front() == stream[position]) {
				found = true;
				break;
			}
		}
		if (!found)
			return std::nullopt;
		position += words.size();
		if (word < position)
			return index;
		state = produced;
	}
	return std::nullopt;
}

void build_native_stream_into(std::vector<std::uintptr_t> &stream,
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links)
{
	stream.clear();
	if (append_native_stream(stream, gadgets, final_rip, mode,
		program_begin, links))
		throw std::logic_error{
			"x86-64 operation has no native assembly gadget"};
}

std::vector<std::uintptr_t> build_native_stream(
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links)
{
	std::vector<std::uintptr_t> stream;
	build_native_stream_into(stream, gadgets, final_rip, mode,
		program_begin, links);
	return stream;
}

bool repeated_string(const Instruction &instruction) noexcept
{
	switch (instruction.operation) {
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan:
		return instruction.repeat != 0;
	default:
		return false;
	}
}

std::vector<std::uintptr_t> build_repeat_step_stream(
	const Gadget &gadget, ExecutionMode mode, std::uint64_t program_begin)
{
	std::vector<std::uintptr_t> stream;
	StreamWords words{stream, mode, program_begin};
	if (!words.position(gadget))
		throw std::logic_error{
			"x86-64 step of an instruction beyond its block's reach"};
	if (!repeated_string(gadget.instruction) ||
		!append_string(words, gadget, true))
		throw std::logic_error{
			"x86-64 step of a non-repeated instruction"};
	stream.push_back(entry_address(native::uml_x86_64_gadget_block_exit));
	stream.push_back(next_instruction_pointer(gadget, mode));
	return stream;
}

void append_leave_stream(std::vector<std::uintptr_t> &stream,
	std::uint64_t rip)
{
	/* Unlinked: a linked exit to its own address would enter itself again
	 * without retiring anything, and never spend the budget. */
	stream.push_back(entry_address(native::uml_x86_64_gadget_block_exit));
	stream.push_back(rip);
}

} // namespace uml::cpu::x86_64::gadgets
