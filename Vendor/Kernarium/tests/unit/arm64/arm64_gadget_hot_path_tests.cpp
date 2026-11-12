// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_return();
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_packed_zero();
void uml_arm64_gadget_move_wide_packed_ones();
extern void (*const uml_arm64_gadget_move_wide_keep_inplace_table[])();
extern void (*const uml_arm64_gadget_scalar_unsigned_inplace_table[])();
void uml_arm64_gadget_add_sub_immediate_x_add_n_rr();
void uml_arm64_gadget_add_sub_immediate_x_add_s_ss();
void uml_arm64_gadget_add_sub_carry_x_adc_n_rr_r();
void uml_arm64_gadget_variable_shift_x_lsl_rr_r();
void uml_arm64_gadget_divide_w_s_rr_r();
void uml_arm64_gadget_load8_unsigned_fast_r();
void uml_arm64_gadget_vector_load128_unsigned_fast();
void uml_arm64_gadget_literal_load64_fast_r();
void uml_arm64_gadget_literal_load64_far_r();
void uml_arm64_gadget_scalar_other_load32u_pre_r();
void uml_arm64_gadget_vector_other_load128_pre();
void uml_arm64_gadget_integer_pair_load64_1_rr();
void uml_arm64_gadget_vector_pair_load128_2();
void uml_arm64_gadget_simd_structure_contig_q_2_load_1();
void uml_arm64_gadget_simd_replicate_s_q_3_0();
void uml_arm64_gadget_simd_lane_h_4_load_2();
void uml_arm64_gadget_ordered_load_acquire_x_r();
void uml_arm64_gadget_atomic_ldadd_relaxed_x_rr();
void uml_arm64_gadget_exclusive_load_relaxed_x_r();
void uml_arm64_gadget_simd_three_same_sshl_8b();
void uml_arm64_gadget_simd_three_same_urshl_2d();
void uml_arm64_gadget_simd_three_same_smaxp_16b();
void uml_arm64_gadget_simd_three_same_add_d();
void uml_arm64_gadget_simd_three_same_mul_4s();
void uml_arm64_gadget_simd_three_same_cmeq_8h();
void uml_arm64_gadget_simd_unary_xtn_8h2();
}

using uml::testing::require;
using uml::emulator::tests::address;

template<typename Function>
static void require_gadget(const gadgets::Program &program, Function expected)
{
	const auto entry = address(expected);
	if (program.words()[0] != entry)
		std::fprintf(stderr, "unexpected gadget: actual=%#lx expected=%#lx\n",
			static_cast<unsigned long>(program.words()[0]),
			static_cast<unsigned long>(entry));
	require(program.words()[0] == entry);
}
static gadgets::Program build_one(std::uint32_t instruction)
{
	constexpr std::uint64_t base = 0x4000;
	const std::array instructions{instruction, 0xd400'0001U};
	auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	return std::move(built.program);
}

static void require_predecoded(const gadgets::Program &program,
	std::uint32_t instruction)
{
	require(program.words()[1] != instruction);
	require(program.words()[2] == 0x4000U);
}

/* The unsigned-offset scalar and integer pair transfers carry no PC word: their
 * one operand ends in the instruction's position in its block, here the first,
 * and the terminal build_one appends follows it directly. */
static void require_positioned(const gadgets::Program &program,
	std::uint32_t instruction)
{
	const std::array terminal{0xd400'0001U};
	const auto alone = gadgets::Builder{}.build(0x4004, terminal);
	require(program.words()[1] != instruction);
	require((program.words()[1] >> 56) == 0U);
	require(program.words().size() == 2U + alone.program.words().size());
}

static constexpr std::uint32_t unsigned_immediate(unsigned size,
	unsigned operation, unsigned immediate, unsigned base,
	unsigned transfer, bool vector = false)
{
	return 0x3900'0000U | ((size & 3U) << 30) |
		((operation & 3U) << 22) | (static_cast<unsigned>(vector) << 26) |
		((immediate & 0xfffU) << 10) | ((base & 31U) << 5) |
		(transfer & 31U);
}

static constexpr std::uint32_t immediate_other(unsigned size,
	unsigned operation, unsigned immediate, unsigned mode, unsigned base,
	unsigned transfer, bool vector = false)
{
	return 0x3800'0000U | ((size & 3U) << 30) |
		((operation & 3U) << 22) | (static_cast<unsigned>(vector) << 26) |
		((immediate & 0x1ffU) << 12) | ((mode & 3U) << 10) |
		((base & 31U) << 5) | (transfer & 31U);
}

static constexpr std::uint32_t pair(unsigned operation, bool vector,
	bool load, unsigned mode, unsigned immediate, unsigned base,
	unsigned first, unsigned second)
{
	return 0x2800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) |
		(static_cast<unsigned>(load) << 22) | ((mode & 3U) << 23) |
		((immediate & 0x7fU) << 15) | ((second & 31U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t simd_multiple(bool q, bool load,
	bool post_index, unsigned offset, unsigned opcode, unsigned size,
	unsigned base, unsigned first)
{
	return 0x0c00'0000U | (static_cast<unsigned>(q) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) | ((offset & 31U) << 16) |
		((opcode & 15U) << 12) | ((size & 3U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t simd_single(bool q, bool load,
	bool post_index, unsigned extra, unsigned offset, unsigned opcode,
	unsigned scale_bit, unsigned size, unsigned base, unsigned first)
{
	return 0x0d00'0000U | (static_cast<unsigned>(q) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) | ((extra & 1U) << 21) |
		((offset & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t atomic(unsigned size, unsigned operation,
	unsigned base, unsigned source, unsigned destination)
{
	return 0x3820'0000U | ((size & 3U) << 30) |
		((source & 31U) << 16) | ((operation & 7U) << 12) |
		((base & 31U) << 5) | (destination & 31U);
}

static constexpr std::uint32_t exclusive_load(unsigned size,
	unsigned base, unsigned transfer)
{
	return 0x0800'0000U | ((size & 3U) << 30) | (1U << 22) |
		(31U << 16) | (31U << 10) | ((base & 31U) << 5) |
		(transfer & 31U);
}

static constexpr std::uint32_t three_same(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode,
	unsigned destination = 0, unsigned left = 1, unsigned right = 2)
{
	return (scalar ? 0x5e20'0400U : 0x0e20'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 11) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

static constexpr std::uint32_t two_register(bool q, unsigned size,
	unsigned opcode, unsigned destination = 0, unsigned source = 1)
{
	return 0x0e20'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

static void mixed_family_walk_preserves_stream_order()
{
	constexpr std::uint64_t base = 0x6000;
	const std::array ordinary{
		0xd503'201fU,
		0xd280'00a0U,
		three_same(false, false, false, 0, 8),
		unsigned_immediate(3, 1, 1, 3, 2),
	};
	constexpr std::uint32_t terminal = 0xd400'0001U;
	std::vector<std::uintptr_t> expected;
	bool accesses_fp_state = false;
	for (std::size_t index = 0; index < ordinary.size(); ++index) {
		const std::array instruction{ordinary[index]};
		const auto built = gadgets::Builder{}.build(
			base + index * sizeof(std::uint32_t), instruction);
		require(built.complete);
		require(built.program.instruction_count() == 1U);
		require(built.program.words().size() >= 3U);
		std::vector<std::uintptr_t> words(built.program.words().begin(),
			built.program.words().end() - 2);
		/* The unsigned-offset load names its instruction by its position in
		 * the block, which is zero when it is built alone. */
		if (index == 3U)
			words[1] |= std::uintptr_t{index} << 56;
		expected.insert(expected.end(), words.begin(), words.end());
		accesses_fp_state = accesses_fp_state ||
			built.program.accesses_fp_state();
	}

	const std::array terminal_instruction{terminal};
	const auto terminal_built = gadgets::Builder{}.build(
		base + ordinary.size() * sizeof(std::uint32_t),
		terminal_instruction);
	expected.insert(expected.end(), terminal_built.program.words().begin(),
		terminal_built.program.words().end());
	accesses_fp_state = accesses_fp_state ||
		terminal_built.program.accesses_fp_state();

	const std::array mixed{
		ordinary[0], ordinary[1], ordinary[2], ordinary[3], terminal};
	const auto built = gadgets::Builder{}.build(base, mixed);
	require(built.complete);
	require(built.stop_address ==
		base + ordinary.size() * sizeof(std::uint32_t));
	require(built.stop_instruction == terminal);
	require(built.program.instruction_count() == mixed.size());
	require(built.program.accesses_fp_state() == accesses_fp_state);
	require(built.program.words().size() == expected.size());
	require(std::equal(built.program.words().begin(),
		built.program.words().end(), expected.begin()));
}

int main()
{
	mixed_family_walk_preserves_stream_order();
	{
		const auto program = build_one(0xd61f'0060U); // br x3
		require_gadget(program, uml_arm64_gadget_branch_register);
		require(program.words()[1] == 3U * 8U);
	}
	{
		const auto program = build_one(0xd63f'0080U); // blr x4
		require_gadget(program, uml_arm64_gadget_branch_link_register);
		require(program.words()[1] == 4U * 8U);
		require(program.words()[2] == 0x4004U);
	}
	{
		const auto program = build_one(0xd65f'03c0U); // ret x30
		require_gadget(program, uml_arm64_gadget_return);
		require(program.words()[1] == 30U * 8U);
	}
	{
		const auto program = build_one(0x1000'0002U); // adr x2, pc
		require_gadget(program, uml_arm64_gadget_move_wide_packed_zero);
		require(program.words()[1] == (std::uint64_t{2} * 8U << 48 | 0x4000U));
		require(program.words()[2] == address(uml_arm64_gadget_svc));
	}
	{
		const auto program = build_one(0x1000'001fU); // adr xzr, pc
		require_gadget(program, uml_arm64_gadget_nop);
		/* No discarded destination operand/value is left in the stream. */
		require(program.words()[1] == address(uml_arm64_gadget_svc));
	}
	{
		const auto program = build_one(0xd280'00a0U); // movz x0, #5
		require(program.words()[0] ==
			address(uml_arm64_gadget_move_wide_packed_zero));
		/* The destination's offset over the value's low 48 bits. */
		require(program.words()[1] == 5U);
		require(program.words()[2] == address(uml_arm64_gadget_svc));
	}
	{
		const auto program = build_one(0xd280'00a3U); // movz x3, #5
		require(program.words()[0] ==
			address(uml_arm64_gadget_move_wide_packed_zero));
		require(program.words()[1] == (std::uint64_t{3} * 8U << 48 | 5U));
	}
	{
		const auto program = build_one(0x9280'0004U); // movn x4, #0
		require(program.words()[0] ==
			address(uml_arm64_gadget_move_wide_packed_ones));
		require(program.words()[1] == (std::uint64_t{4} * 8U << 48 |
			0xffff'ffff'ffffU));
	}
	{
		const auto program = build_one(0xd2e0'0025U); // movz x5, #1, lsl #48
		/* Neither a 48-bit value nor one of ones above bit 47. */
		require(program.words()[0] ==
			address(uml_arm64_gadget_move_wide_constant));
		require(program.words()[1] == 5U * 8U);
		require(program.words()[2] == std::uint64_t{1} << 48);
	}
	{
		const auto program = build_one(0xf2a2'4680U); // movk x0,#0x1234,lsl#16
		/* MOVK is in place by construction, so the baked entry for the
		 * destination register is what the translator selects. Rows are
		 * w0 w1 x0 x1 x2 x3; this is x1, destination x0. */
		require(program.words()[0] == address(
			uml_arm64_gadget_move_wide_keep_inplace_table[3U * 31U + 0U]));
	}
	{
		const auto program = build_one(0x9100'1c01U); // add x1,x0,#7
		require(program.words()[0] ==
			address(uml_arm64_gadget_add_sub_immediate_x_add_n_rr));
	}
	{
		const auto program = build_one(0xb100'07ffU); // adds xzr,sp,#1
		require(program.words()[0] ==
			address(uml_arm64_gadget_add_sub_immediate_x_add_s_ss));
	}
	{
		const auto program = build_one(0x9a04'0062U); // adc x2,x3,x4
		require(program.words()[0] ==
			address(uml_arm64_gadget_add_sub_carry_x_adc_n_rr_r));
	}
	{
		const auto program = build_one(0x9ac4'2062U); // lslv x2,x3,x4
		require(program.words()[0] ==
			address(uml_arm64_gadget_variable_shift_x_lsl_rr_r));
	}
	{
		const auto program = build_one(0x1ac4'0c62U); // sdiv w2,w3,w4
		require(program.words()[0] ==
			address(uml_arm64_gadget_divide_w_s_rr_r));
	}
	{
		const auto program = build_one(0xf940'0462U); // ldr x2,[x3,#8]
		/* The transferred register is baked, so the translator selects the
		 * entry for x2. Index is ((operation * 2 + bits64) * 2 + base_sp)
		 * * 31 + register, with operation 1 for a load. */
		require(program.words()[0] == address(
			uml_arm64_gadget_scalar_unsigned_inplace_table[
				((1U * 2U + 1U) * 2U + 0U) * 31U + 2U]));
		/* The opcode is not carried into the runtime stream, and neither is
		 * a PC: one operand, the offsets, displacement and position. */
		require(program.words()[1] == (2U * 8U | (3U * 8U) << 16 |
			std::uintptr_t{8} << 32));
		require_positioned(program, 0xf940'0462U);
	}
	{
		const auto instruction = unsigned_immediate(0, 1, 1, 3, 2);
		const auto program = build_one(instruction); // ldrb w2,[x3,#1]
		require_gadget(program, uml_arm64_gadget_load8_unsigned_fast_r);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = unsigned_immediate(0, 3, 1, 3, 2, true);
		const auto program = build_one(instruction); // ldr q2,[x3,#16]
		require_gadget(program, uml_arm64_gadget_vector_load128_unsigned_fast);
		require_positioned(program, instruction);
	}
	{
		constexpr std::uint32_t instruction = 0x5800'0042U; // ldr x2,pc+8
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_literal_load64_fast_r);
		/* The operand holds the target, the destination offset and the
		 * position, and nothing follows it. */
		require(program.words()[1] == (std::uintptr_t{0x4008} |
			std::uintptr_t{2U * 8U} << 48));
		require_positioned(program, instruction);
	}
	{
		/* A target the 48-bit field cannot hold keeps the PC word. */
		constexpr std::uint32_t instruction = 0x58fd'ffe2U; // ldr x2,pc-0x4004
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_literal_load64_far_r);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = immediate_other(2, 1, 1, 3, 3, 2);
		const auto program = build_one(instruction); // ldr w2,[x3,#1]!
		require_gadget(program, uml_arm64_gadget_scalar_other_load32u_pre_r);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = immediate_other(0, 3, 1, 3, 3, 2, true);
		const auto program = build_one(instruction); // ldr q2,[x3,#1]!
		require_gadget(program, uml_arm64_gadget_vector_other_load128_pre);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = pair(2, false, true, 3, 1, 3, 2, 4);
		const auto program = build_one(instruction); // ldp x2,x4,[x3,#8]!
		require_gadget(program, uml_arm64_gadget_integer_pair_load64_1_rr);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = pair(2, true, true, 1, 1, 3, 2, 4);
		const auto program = build_one(instruction); // ldp q2,q4,[x3],#16
		require_gadget(program, uml_arm64_gadget_vector_pair_load128_2);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = simd_multiple(true, true, true,
			31, 10, 0, 3, 2); // ld1 {v2.16b,v3.16b},[x3],#32
		const auto program = build_one(instruction);
		require_gadget(program,
			uml_arm64_gadget_simd_structure_contig_q_2_load_1);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = simd_single(true, true, false,
			0, 0, 7, 0, 2, 3, 2); // ld3r {v2.4s-v4.4s},[x3]
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_simd_replicate_s_q_3_0);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = simd_single(true, true, true,
			1, 4, 3, 0, 0, 3, 2); // ld4 lanes, post-index x4
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_simd_lane_h_4_load_2);
		require_positioned(program, instruction);
	}
	{
		constexpr std::uint32_t instruction = 0xc8df'fc62U;
		const auto program = build_one(instruction); // ldar x2,[x3]
		require_gadget(program, uml_arm64_gadget_ordered_load_acquire_x_r);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = atomic(3, 0, 3, 4, 2);
		const auto program = build_one(instruction); // ldadd x4,x2,[x3]
		require_gadget(program, uml_arm64_gadget_atomic_ldadd_relaxed_x_rr);
		require_positioned(program, instruction);
	}
	{
		const auto instruction = exclusive_load(3, 3, 2);
		const auto program = build_one(instruction); // ldxr x2,[x3]
		require_gadget(program, uml_arm64_gadget_exclusive_load_relaxed_x_r);
		require_positioned(program, instruction);
	}
	{
		const auto program = build_one(three_same(false, false, false, 0, 8));
		require_gadget(program, uml_arm64_gadget_simd_three_same_sshl_8b);
	}
	{
		const auto program = build_one(three_same(false, true, true, 3, 10));
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_three_same_urshl_2d));
	}
	{
		const auto program = build_one(three_same(false, true, false, 0, 20));
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_three_same_smaxp_16b));
	}
	{
		const auto program = build_one(three_same(true, false, false, 3, 16));
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_three_same_add_d));
	}
	{
		const auto program = build_one(three_same(false, true, false, 2, 19));
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_three_same_mul_4s));
	}
	{
		const auto program = build_one(three_same(false, true, true, 1, 17));
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_three_same_cmeq_8h));
	}
	{
		const auto program = build_one(two_register(true, 0, 18)); // XTN2
		require(program.words()[0] ==
			address(uml_arm64_gadget_simd_unary_xtn_8h2));
	}
	return 0;
}
