// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu_state.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::x86_64 {

enum class OperandWidth : std::uint8_t {
	byte = 1,
	word = 2,
	dword = 4,
	qword = 8,
	xmm = 16,
	ymm = 32,
};

enum class Segment : std::uint8_t {
	none,
	fs,
	gs,
};

/* A compatibility-mode operand's ES, CS, SS or DS override, or the ES a
 * string destination always uses; long mode ignores them. FS and GS are
 * `Segment`, and an operand has at most one of the two. */
enum class LegacySegment : std::uint8_t {
	none,
	es,
	cs,
	ss,
	ds,
};

enum class OperandKind : std::uint8_t {
	none,
	reg,
	vector_reg,
	mmx_reg,
	memory,
	immediate,
};

struct RegisterOperand final {
	std::uint8_t index{};
	bool high_byte{};
};

struct MemoryOperand final {
	static constexpr std::uint8_t no_register = 0xff;

	std::uint8_t base{no_register};
	std::uint8_t index{no_register};
	std::uint8_t scale{1};
	std::int32_t displacement{};
	std::uint64_t absolute{};
	Segment segment{Segment::none};
	LegacySegment legacy_segment{LegacySegment::none};
	/* Never decoded: set by the runtime for an instruction of a task whose
	 * ES, SS or DS has a base or a limit (`segments.hpp`), which it runs
	 * alone after checking it. The address then adds the base of the
	 * operand's ES, CS, SS or DS. */
	bool segment_base{};
	bool rip_relative{};
	bool address_16{};
	bool address_32{};
	bool absolute_address{};
	bool vector_index{};
};

struct Operand final {
	OperandKind kind{OperandKind::none};
	OperandWidth width{OperandWidth::qword};
	RegisterOperand reg{};
	MemoryOperand memory{};
	std::uint64_t immediate{};
};

enum class Operation : std::uint8_t {
	nop,
	move,
	move_zero_extend,
	move_sign_extend,
	lea,
	exchange,
	add,
	add_carry,
	subtract,
	subtract_borrow,
	increment,
	decrement,
	negate,
	bit_not,
	bit_and,
	bit_or,
	bit_xor,
	compare,
	test,
	shift_left,
	shift_right,
	shift_arithmetic_right,
	rotate_left,
	rotate_right,
	rotate_carry_left,
	rotate_carry_right,
	double_shift_left,
	double_shift_right,
	multiply,
	multiply_signed,
	divide,
	divide_signed,
	bit_test,
	bit_set,
	bit_reset,
	bit_complement,
	bit_scan_forward,
	bit_scan_reverse,
	count_trailing_zeros,
	count_leading_zeros,
	population_count,
	adjust_rpl,
	compare_exchange,
	compare_exchange_pair,
	exchange_add,
	move_byte_swap,
	crc32,
	adx,
	scalar_bmi,
	set_condition,
	conditional_move,
	byte_swap,
	sign_extend_accumulator,
	sign_extend_data,
	push,
	pop,
	push_all,
	pop_all,
	push_flags,
	pop_flags,
	leave,
	enter,
	string_move,
	string_compare,
	string_store,
	string_load,
	string_scan,
	carry_clear,
	carry_set,
	carry_complement,
	direction_clear,
	direction_set,
	lahf,
	sahf,
	legacy_adjust,
	interrupt_overflow,
	segment_move,
	segment_stack,
	jump,
	jump_indirect,
	jump_conditional,
	loop,
	call,
	call_indirect,
	far_call,
	far_jump,
	ret,
	far_ret,
	interrupt_ret,
	xlat,
	cpuid,
	read_extended_control,
	read_timestamp,
	read_processor_id,
	random,
	read_fs_base,
	read_gs_base,
	write_fs_base,
	write_gs_base,
	store_gdtr,
	store_machine_status,
	load_segment_limit,
	memory_fence,
	cache_flush,
	vector_move,
	vector_move_scalar,
	vector_zero,
	vector_alu,
	vector_lane_move,
	vector_convert,
	vector_gpr_move,
	vector_integer,
	vector_integer_map2,
	vector_integer_map3,
	vector_fma,
	vector_dot,
	vector_map2_misc,
	vector_gather,
	vector_test,
	vector_aes,
	vector_sha,
	vector_to_gpr,
	vector_insert,
	vector_scalar_convert,
	vector_compare_flags,
	vector_mask_store,
	x87,
	extended_state,
	mxcsr,
	system_call,
	breakpoint,
	/* ICEBP (INT1): #DB as a trap after the instruction. */
	debug_trap,
	/* BOUND, outside 64-bit mode: #BR when the signed index in the
	 * destination lies outside the pair of bounds in memory. */
	bound,
	/* Raises #GP at user privilege: port I/O, HLT, CLI/STI, INT n, MSRs. */
	privileged,
	/* LDS, LES, LSS, LFS and LGS: the offset of the far pointer in memory
	 * to the destination register, its selector to the segment register
	 * `vector_group` numbers, ES 0 to GS 5. */
	load_far_pointer,
	/* LAR: the access rights of the descriptor the source selector names,
	 * with ZF set, or ZF clear. */
	load_access_rights,
	/* VERR (`vector_group` 4) and VERW (5): ZF says whether the source
	 * selector names a segment CPL 3 could read or write. */
	verify_segment,
	/* SLDT (`vector_group` 0) and STR (1), which Linux emulates for user
	 * code under UMIP. */
	store_segment_table,
};

enum class Condition : std::uint8_t {
	overflow,
	not_overflow,
	below,
	above_or_equal,
	equal,
	not_equal,
	below_or_equal,
	above,
	sign,
	not_sign,
	parity,
	not_parity,
	less,
	greater_or_equal,
	less_or_equal,
	greater,
};

struct Instruction final {
	Operation operation{Operation::nop};
	Operand destination{};
	Operand source{};
	Operand auxiliary{};
	Condition condition{Condition::equal};
	std::uint8_t length{};
	std::uint8_t repeat{};
	std::uint8_t vector_opcode{};
	std::uint8_t vector_map{};
	std::uint8_t vector_prefix{};
	std::uint8_t vector_control{};
	std::uint8_t vector_group{};
	bool locked{};
	bool vex{};

	[[nodiscard]] bool terminal() const noexcept;
};

enum class DecodeStatus : std::uint8_t {
	decoded,
	fetch_fault,
	illegal_instruction,
};

struct DecodeResult final {
	DecodeStatus status{DecodeStatus::illegal_instruction};
	Instruction instruction{};
	std::uint64_t fault_address{};
};

/* x86 encodes no instruction longer than this. An encoding that would need
 * another byte decodes as a privileged operation, which raises #GP at its
 * first byte as the hardware does, without fetching that byte. */
inline constexpr std::size_t maximum_instruction_bytes = 15;

/* Guest bytes a decode has already fetched. A caller decoding a run of
 * instructions hands the same window to each of them, so a block is fetched
 * once per window rather than once per instruction. The window never spans a
 * guest page, which is what keeps it unable to fault for a page the decode has
 * not reached; it is refilled from the address a decode wants whenever that
 * address is not already in it, so a caller may hand a stale one to any
 * address. */
struct FetchWindow final {
	static constexpr std::size_t capacity = 128;

	std::uint64_t address{};
	std::uint32_t filled{};
	std::array<std::byte, capacity> bytes{};
};

/* `consumed`, when it is not null, must address at least
 * `maximum_instruction_bytes` writable bytes and receives every byte this call
 * read from guest memory, in the order it read them. On a decoded result the
 * first `instruction.length` of them are exactly the instruction's encoding,
 * because the length is the reader's position; bytes past it were fetched with
 * the same window and belong to no instruction. A caller that keeps a
 * translation saves those bytes rather than reading the instruction stream a
 * second time: a second read can see another vCPU's store that the decode did
 * not, and nothing downstream could tell the two apart afterwards.
 *
 * The reader fetches in windows bounded by the guest page rather than one byte
 * per call, so an instruction is normally one observation of guest memory
 * instead of up to fifteen, and never fewer pages than it reaches. `window`,
 * when it is given, is reused across calls, so a block's bytes are fetched
 * once for the run of instructions that fits in it -- the same single
 * observation, made earlier.
 */
[[nodiscard]] DecodeResult decode(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t rip,
	ExecutionMode mode = ExecutionMode::long64,
	std::byte *consumed = nullptr, FetchWindow *window = nullptr) noexcept;

/* Decode one instruction out of bytes the caller already read, `code`
 * beginning at the instruction's first byte. Those bytes stand in for the
 * window a fetch would fill, so this decode reads guest memory not at all and
 * cannot fault against it: a translation handed its own saved source decodes
 * exactly the instruction it was built from, whatever has happened to that
 * memory since. `rip` names where the instruction sits, which is what a
 * shortfall is reported against; an encoding that reaches past the bytes it
 * was given reports a fetch fault there, the same answer the same shortfall
 * gets from guest memory.
 */
[[nodiscard]] DecodeResult decode(std::span<const std::byte> code,
	std::uint64_t rip, ExecutionMode mode = ExecutionMode::long64) noexcept;

} // namespace uml::cpu::x86_64
