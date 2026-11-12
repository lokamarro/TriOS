// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/decode_support.hpp"
#include "cpu/arm64/aarch32/form_operation.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

/*
 * What the AArch32 semantic library's sources share (semantics.hpp): the
 * library's table of the decoder's forms, the operand context of one
 * instruction, and the handler each family of operations defines.
 */

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#error "the guest page profile sizes the library's page splits"
#endif

namespace uml::cpu::arm64::aarch32::detail {

inline constexpr std::uint32_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;

/* The addressing mode of a multiple transfer: increment after or before,
 * decrement after or before. */
enum class Mode : std::uint8_t { ia, ib, da, db };

/* The shape of a VLDn or VSTn form: multiple n-element structures, a single
 * structure to one lane, or one structure to all lanes (loads only). */
enum class StructureKind : std::uint8_t { multiple, single_lane, all_lanes };

class Exec;

/* A handler for the VFP and Advanced SIMD forms that share a pseudocode
 * body (one XML file); it executes an instruction whose condition has
 * passed, as the family handlers do. */
using VectorHandler = bool (*)(Exec &x);

/* A form's operation (form_operation.hpp) and what the library needs to
 * execute it. */
struct OpInfo final : FormOperation {
	/* A multiple transfer's mode, and whether it is PUSH or POP, whose base
	 * is SP with writeback. */
	Mode transfer{Mode::ia};
	bool stack{};
	/* A VLDn or VSTn: n, from 1 to 4, and the shape. */
	std::uint8_t structs{};
	StructureKind structure{StructureKind::multiple};
	/* For Op::vector: the handler. */
	VectorHandler vector{};
};

/* A stem of the decoder's form names (the name up to its encoding tag, as
 * VADD_f for VADD_f_A1_D) and the handler of the forms that share it. */
struct VectorEntry final {
	const char *stem;
	VectorHandler handler;
};

/* The handlers each source file defines. */
[[nodiscard]] std::span<const VectorEntry> vector_move_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> neon_integer_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> neon_shift_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> vector_float_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> armv8_float_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> armv8_integer_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> crypto_entries() noexcept;
[[nodiscard]] std::span<const VectorEntry> bf16_entries() noexcept;

[[nodiscard]] const OpInfo &op_info(Form form) noexcept;

/* Whether the library executes a form of this table row and operation. */
[[nodiscard]] bool covered(const FormInfo &form, const OpInfo &info) noexcept;

inline constexpr unsigned reg_sp = 13;
inline constexpr unsigned reg_lr = 14;
inline constexpr unsigned reg_pc = 15;

/* The state of one executing instruction: the registers it reads and
 * writes, the PC arithmetic of its instruction set, the memory it reaches
 * and the event it raises. A handler returns true when the instruction
 * completed and false when it raised an event in `result`. */
class Exec final {
public:
	Exec(CpuState &cpu_state, const Instruction &decoded,
		A32Memory &memory) noexcept
		: cpu(cpu_state), inst(decoded), o(decoded.operands),
		  mem(memory), thumb(key_is_thumb(cpu_state.pc)),
		  pc(key_pc(cpu_state.pc))
	{
	}

	CpuState &cpu;
	const Instruction &inst;
	const Operands &o;
	A32Memory &mem;
	/* The instruction set the instruction runs in, and its address. */
	const bool thumb;
	const std::uint32_t pc;
	StepResult result{};
	/* A handler wrote the PC (a branch or an interworking load). */
	bool pc_written{};

	/* The value Arm's PC32() reads: the instruction's address plus 8 in A32
	 * and 4 in T32. */
	[[nodiscard]] std::uint32_t pc_value() const noexcept
	{
		return pc + (thumb ? 4U : 8U);
	}
	[[nodiscard]] std::uint32_t pc_aligned() const noexcept
	{
		return pc_value() & ~3U;
	}

	/* R[n]: a read of the PC sees PC32(). */
	[[nodiscard]] std::uint32_t reg(unsigned n) const noexcept
	{
		return n == reg_pc ? pc_value()
				   : static_cast<std::uint32_t>(cpu.registers[n]);
	}
	/* R[n] = value, for n below 15; the PC is written through the branch
	 * helpers. */
	void set_reg(unsigned n, std::uint32_t value) noexcept
	{
		cpu.registers[n] = value;
	}
	[[nodiscard]] std::uint32_t reg(std::int32_t n) const noexcept
	{
		return reg(static_cast<unsigned>(n));
	}
	void set_reg(std::int32_t n, std::uint32_t value) noexcept
	{
		set_reg(static_cast<unsigned>(n), value);
	}

	/* The VFP and Advanced SIMD register file: D0-D31 overlay Q0-Q15 in
	 * vectors[0..15] (D2n is the low half of vectors[n]), S0-S31 the halves
	 * of D0-D15. */
	[[nodiscard]] std::uint64_t d_reg(unsigned n) const noexcept
	{
		std::uint64_t value;
		std::memcpy(&value, cpu.vectors[n >> 1].bytes.data() + 8U * (n & 1U), 8);
		return value;
	}
	void set_d_reg(unsigned n, std::uint64_t value) noexcept
	{
		std::memcpy(cpu.vectors[n >> 1].bytes.data() + 8U * (n & 1U), &value, 8);
	}
	[[nodiscard]] std::uint32_t s_reg(unsigned n) const noexcept
	{
		std::uint32_t value;
		std::memcpy(&value, cpu.vectors[n >> 2].bytes.data() + 4U * (n & 3U), 4);
		return value;
	}
	void set_s_reg(unsigned n, std::uint32_t value) noexcept
	{
		std::memcpy(cpu.vectors[n >> 2].bytes.data() + 4U * (n & 3U), &value, 4);
	}
	/* The bytes of D(n), for the element accesses. */
	[[nodiscard]] std::byte *d_bytes(unsigned n) noexcept
	{
		return cpu.vectors[n >> 1].bytes.data() + 8U * (n & 1U);
	}

	/* The condition flags. */
	[[nodiscard]] bool flag_n() const noexcept { return (cpu.nzcv >> 31 & 1U) != 0U; }
	[[nodiscard]] bool flag_z() const noexcept { return (cpu.nzcv >> 30 & 1U) != 0U; }
	[[nodiscard]] bool flag_c() const noexcept { return (cpu.nzcv >> 29 & 1U) != 0U; }
	[[nodiscard]] bool flag_v() const noexcept { return (cpu.nzcv >> 28 & 1U) != 0U; }
	void set_nzcv(bool n, bool z, bool c, bool v) noexcept
	{
		cpu.nzcv = (cpu.nzcv & 0x0fff'ffffU) | (n ? 0x8000'0000U : 0U)
			| (z ? 0x4000'0000U : 0U) | (c ? 0x2000'0000U : 0U)
			| (v ? 0x1000'0000U : 0U);
	}
	/* N and Z from a result, C from `carry`, V unchanged. */
	void set_nzc(std::uint32_t value, bool carry) noexcept
	{
		set_nzcv((value >> 31) != 0U, value == 0U, carry, flag_v());
	}
	void set_q() noexcept { cpu.a32_q = 1; }

	/* PSTATE.GE[lane], and the four flags from their byte-lane mask form. */
	[[nodiscard]] bool ge(unsigned lane) const noexcept
	{
		return (cpu.a32_ge_mask >> (8U * lane) & 0xffU) != 0U;
	}
	void set_ge_lane(unsigned lane, bool value) noexcept
	{
		cpu.a32_ge_mask = (cpu.a32_ge_mask & ~(0xffU << (8U * lane)))
			| (value ? 0xffU << (8U * lane) : 0U);
	}

	/* ConditionPassed() for the instruction's condition. */
	[[nodiscard]] bool condition_passed() const noexcept
	{
		return condition_holds(inst.cond);
	}
	/* ConditionHolds(): a condition code, 0 EQ to 14 AL, on the flags. */
	[[nodiscard]] bool condition_holds(unsigned cond) const noexcept;

	/* BranchWritePC(): the PC is word aligned in A32 and halfword aligned
	 * in T32, in the current instruction set. */
	void branch_write_pc(std::uint32_t address) noexcept
	{
		branch_to(thumb, address);
	}
	/* BranchWritePC() after SelectInstrSet(): a BL or BLX immediate. */
	void branch_to(bool to_thumb, std::uint32_t address) noexcept
	{
		cpu.pc = to_thumb ? t32_key(address & ~1U) : a32_key(address & ~3U);
		pc_written = true;
	}
	/* BXWritePC(): bit 0 of the target selects T32. An A32 target keeps bit
	 * 1, so the next fetch is a PC alignment fault. */
	void bx_write_pc(std::uint32_t address) noexcept
	{
		cpu.pc = key_from_target(address);
		pc_written = true;
	}
	/* ALUWritePC(): the interworking branch in A32 and the plain one in
	 * T32. */
	void alu_write_pc(std::uint32_t address) noexcept
	{
		if (thumb)
			branch_write_pc(address);
		else
			bx_write_pc(address);
	}
	/* LoadWritePC(): always interworking. */
	void load_write_pc(std::uint32_t address) noexcept { bx_write_pc(address); }

	/* The address of the next instruction. */
	[[nodiscard]] std::uint32_t next_pc() const noexcept
	{
		return pc + inst.length;
	}

	/* Events. */
	bool raise_undefined() noexcept
	{
		result.event = StepEvent::undefined;
		return false;
	}
	bool raise_fatal() noexcept
	{
		result.event = StepEvent::fatal;
		return false;
	}
	bool raise_memory(std::uint32_t address, ::uml::memory::MemoryAccess access,
		bool alignment) noexcept
	{
		result.event = StepEvent::memory_fault;
		result.alignment = alignment;
		result.access = access;
		result.fault_address = address;
		return false;
	}
	bool raise_fault(std::uint32_t address,
		::uml::memory::MemoryAccess access) noexcept
	{
		return raise_memory(address, access, false);
	}
	bool raise_alignment(std::uint32_t address,
		::uml::memory::MemoryAccess access) noexcept
	{
		return raise_memory(address, access, true);
	}

	/* Memory. read() and write() are single accesses of up to a page
	 * (callers check alignment first): a read fills `destination` or faults
	 * at the first byte of the page that fails to translate; a write
	 * translates every page it touches before storing any byte. */
	bool read(std::uint32_t address, void *destination, std::size_t size) noexcept;
	bool write(std::uint32_t address, const void *source, std::size_t size) noexcept;
	/* A store of consecutive words in the pseudocode's order: stores the
	 * words that translate, in order, and stops at the first that does not,
	 * which faults at its address. */
	bool write_words(std::uint32_t address, const std::uint32_t *words,
		unsigned count) noexcept;
	/* The host address of an aligned access of `size` bytes, for an atomic
	 * operation, or null after raising the fault. */
	std::byte *atomic_pointer(std::uint32_t address, unsigned size,
		::uml::memory::MemoryAccess access) noexcept;
	void note_stored(std::byte *host, std::size_t size) noexcept
	{
		mem.stored(host, size);
	}
};

/* The handlers, one source per family. Each executes an operation whose
 * condition has passed. */
bool exec_data_processing(Exec &x, const OpInfo &info);
bool exec_bits_and_extend(Exec &x, const OpInfo &info);
bool exec_multiply(Exec &x, const OpInfo &info);
bool exec_saturate_and_parallel(Exec &x, const OpInfo &info);
bool exec_branch(Exec &x, const OpInfo &info);
bool exec_single_transfer(Exec &x, const OpInfo &info);
bool exec_multiple_transfer(Exec &x, const OpInfo &info);
bool exec_exclusive(Exec &x, const OpInfo &info);
bool exec_system(Exec &x, const OpInfo &info);
bool exec_vector_memory(Exec &x, const OpInfo &info);

/* Arm's shift helpers (Shift_C, AddWithCarry). */
struct ShiftResult final {
	std::uint32_t value;
	bool carry;
};

[[nodiscard]] constexpr ShiftResult shift_c(std::uint32_t value, SRType type,
	unsigned amount, bool carry_in) noexcept
{
	if (amount == 0 && type != SRType::RRX)
		return {value, carry_in};
	switch (type) {
	case SRType::LSL:
		if (amount > 32U)
			return {0U, false};
		if (amount == 32U)
			return {0U, (value & 1U) != 0U};
		return {value << amount, (value >> (32U - amount) & 1U) != 0U};
	case SRType::LSR:
		if (amount > 32U)
			return {0U, false};
		if (amount == 32U)
			return {0U, (value >> 31) != 0U};
		return {value >> amount, (value >> (amount - 1U) & 1U) != 0U};
	case SRType::ASR: {
		const auto signed_value = static_cast<std::int32_t>(value);
		if (amount >= 32U)
			return {static_cast<std::uint32_t>(signed_value >> 31),
				(value >> 31) != 0U};
		return {static_cast<std::uint32_t>(signed_value >> amount),
			(value >> (amount - 1U) & 1U) != 0U};
	}
	case SRType::ROR: {
		const unsigned rotate = amount & 31U;
		if (rotate == 0U)
			return {value, (value >> 31) != 0U};
		const auto result = std::rotr(value, static_cast<int>(rotate));
		return {result, (result >> 31) != 0U};
	}
	case SRType::RRX:
		return {(carry_in ? 0x8000'0000U : 0U) | value >> 1,
			(value & 1U) != 0U};
	}
	return {value, carry_in};
}

struct AddResult final {
	std::uint32_t value;
	bool carry;
	bool overflow;
};

[[nodiscard]] constexpr AddResult add_with_carry(std::uint32_t x, std::uint32_t y,
	bool carry_in) noexcept
{
	const std::uint64_t unsigned_sum =
		std::uint64_t{x} + std::uint64_t{y} + (carry_in ? 1U : 0U);
	const std::int64_t signed_sum =
		std::int64_t{static_cast<std::int32_t>(x)}
		+ std::int64_t{static_cast<std::int32_t>(y)} + (carry_in ? 1 : 0);
	const auto value = static_cast<std::uint32_t>(unsigned_sum);
	return {value, unsigned_sum != value,
		signed_sum != static_cast<std::int32_t>(value)};
}

/* SignedSatQ() and UnsignedSatQ() of a value, to `bits` bits. */
struct Saturated final {
	std::int64_t value;
	bool saturated;
};

[[nodiscard]] constexpr Saturated signed_sat(std::int64_t value, unsigned bits) noexcept
{
	const std::int64_t maximum = (std::int64_t{1} << (bits - 1U)) - 1;
	const std::int64_t minimum = -(std::int64_t{1} << (bits - 1U));
	if (value > maximum)
		return {maximum, true};
	if (value < minimum)
		return {minimum, true};
	return {value, false};
}

[[nodiscard]] constexpr Saturated unsigned_sat(std::int64_t value, unsigned bits) noexcept
{
	const std::int64_t maximum = (std::int64_t{1} << bits) - 1;
	if (value > maximum)
		return {maximum, true};
	if (value < 0)
		return {0, true};
	return {value, false};
}

} // namespace uml::cpu::arm64::aarch32::detail
