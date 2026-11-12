// SPDX-License-Identifier: GPL-2.0-or-later
/* The streams the builder emits for each conditional branch: the entry the
 * selection tables name, its operand, and the two edges as block links or as
 * guest addresses. Covers the fused CMP + B.cond pairs, CBZ, CBNZ, TBZ, TBNZ
 * and B.cond, each linked, unlinked and with the inline suffix table full, and
 * the branches on XZR that fold to an unconditional transfer. */
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_selection_api.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

extern "C" void uml_arm64_gadget_branch_always();
extern "C" void uml_arm64_gadget_branch_always_linked();

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;
namespace selection = gadgets::selection;

constexpr auto base = engine_test_code_base;
constexpr std::uint32_t nop = 0xd503'201fU;
constexpr std::size_t inline_suffix_slots = gadgets::InlineSuffixes::capacity;
/* The always forms, conditions 14 and 15, have no inline or fused entry. */
constexpr unsigned selectable_conditions = 14;

/* Byte displacements of the 19-bit and the 14-bit branch immediates: a short
 * branch forward and backward, then each extreme of the field. */
constexpr std::array<std::int64_t, 4> long_displacements{
	0x40, -0x20, 0xffffc, -0x100000};
constexpr std::array<std::int64_t, 4> short_displacements{
	0x40, -0x20, 0x7ffc, -0x8000};

/* The guest address `displacement` bytes from the instruction at `index`. */
[[nodiscard]] constexpr std::uint64_t at(
	std::size_t index, std::int64_t displacement = 0)
{
	return base + 4U * index + static_cast<std::uint64_t>(displacement);
}

/* The block link the builder names for each guest address. Targets are far
 * apart, so cells repeat, but one branch's target and fall-through never
 * share one. */
class Cells final {
public:
	[[nodiscard]] gadgets::Builder::LinkResolver resolver()
	{
		return [this](std::uint64_t pc) { return &cells_[index(pc)]; };
	}

	[[nodiscard]] std::uintptr_t word(std::uint64_t pc) const
	{
		return reinterpret_cast<std::uintptr_t>(&cells_[index(pc)]);
	}

private:
	[[nodiscard]] static std::size_t index(std::uint64_t pc)
	{
		return static_cast<std::size_t>(
			((pc - base) / sizeof(std::uint32_t)) % 1024U);
	}

	std::array<gadgets::BlockLink, 1024> cells_{};
};

[[nodiscard]] gadgets::BuildResult build(
	const std::vector<std::uint32_t> &words, Cells *cells)
{
	return gadgets::Builder{}.build(base, words,
		cells != nullptr ? cells->resolver() :
			gadgets::Builder::LinkResolver{});
}

[[nodiscard]] std::uintptr_t entry(selection::Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned index)
{
	return offsetof(arm64::CpuState, registers) + index * 8U;
}

/* CMP Xn/Wn, #imm12 {, LSL #12} and CMP Xn/Wn, Xm/Wm. */
[[nodiscard]] constexpr std::uint32_t compare_immediate(
	bool wide, unsigned source, unsigned immediate, bool shifted)
{
	return (wide ? 0xf100'001fU : 0x7100'001fU) |
		(shifted ? 0x0040'0000U : 0U) | (immediate << 10) | (source << 5);
}

[[nodiscard]] constexpr std::uint32_t compare_register(
	bool wide, unsigned left, unsigned right)
{
	return (wide ? 0xeb00'001fU : 0x6b00'001fU) | (right << 16) |
		(left << 5);
}

/* The operands of the compares above, as the fused entries read them. */
[[nodiscard]] constexpr std::uintptr_t immediate_operand(
	unsigned source, std::uintptr_t immediate)
{
	return 31U | (std::uintptr_t{source} << 8) | (immediate << 16);
}

[[nodiscard]] constexpr std::uintptr_t register_operand(
	unsigned left, unsigned right)
{
	return offsetof(arm64::CpuState, sp) |
		(std::uintptr_t{left * 8U} << 16) |
		(std::uintptr_t{right * 8U} << 32);
}

/* Sixty-four CBZ take every slot of the inline suffix table, three stream
 * words each, so the next conditional branch ends the block. */
[[nodiscard]] std::vector<std::uint32_t> full_table()
{
	return std::vector<std::uint32_t>(inline_suffix_slots,
		encode_compare_branch(true, false, 8, 1));
}

constexpr std::size_t full_table_words = 3U * inline_suffix_slots;

void unlinked_fused_pairs(std::int64_t displacement)
{
	for (const bool wide : {false, true})
		for (unsigned condition = 0; condition < selectable_conditions;
		     ++condition) {
			const auto branch = encode_conditional_branch(
				false, condition, displacement);
			const auto target = at(1, displacement);
			const auto fallthrough = at(2);
			for (const bool shifted : {false, true}) {
				const auto built = build({compare_immediate(
					wide, 3, 5, shifted), branch, nop}, nullptr);
				const auto words = built.program.words();
				require(built.complete && words.size() == 4U &&
						built.program.instruction_count() == 2U,
					"unlinked CMP immediate + B.cond was not one terminal entry");
				require(words[0] == entry(selection::
						compare_immediate_branch_gadget(
							wide, condition, false)) &&
						words[1] == immediate_operand(
							3, shifted ? 5U << 12 : 5U) &&
						words[2] == target &&
						words[3] == fallthrough,
					"unlinked CMP immediate + B.cond fields changed");
			}
			const auto built = build({compare_register(wide, 3, 4),
				branch, nop}, nullptr);
			const auto words = built.program.words();
			require(built.complete && words.size() == 4U &&
					built.program.instruction_count() == 2U,
				"unlinked CMP register + B.cond was not one terminal entry");
			require(words[0] == entry(selection::
					compare_register_branch_gadget(
						wide, condition, false)) &&
					words[1] == register_operand(3, 4) &&
					words[2] == target && words[3] == fallthrough,
				"unlinked CMP register + B.cond fields changed");
		}
}

void linked_fused_pairs_inline_their_fall_through(std::int64_t displacement)
{
	for (const bool wide : {false, true})
		for (unsigned condition = 0; condition < selectable_conditions;
		     ++condition) {
			const auto branch = encode_conditional_branch(
				false, condition, displacement);
			/* The two instructions behind the pair are the suffix. */
			Cells cells;
			auto built = build({compare_immediate(wide, 3, 5, false),
				branch, nop, nop}, &cells);
			auto words = built.program.words();
			require(built.complete && built.program.instruction_count() == 4U,
				"linked CMP immediate + B.cond did not translate the block");
			require(words[0] == entry(selection::
					compare_immediate_branch_inline_gadget(
						wide, condition)) &&
					words[1] == (immediate_operand(3, 5) | (2ULL << 40)) &&
					words[2] == cells.word(at(1, displacement)),
				"linked CMP immediate + B.cond inline fields changed");

			built = build({compare_register(wide, 3, 4), branch, nop, nop},
				&cells);
			words = built.program.words();
			require(built.complete && built.program.instruction_count() == 4U,
				"linked CMP register + B.cond did not translate the block");
			require(words[0] == entry(selection::
					compare_register_branch_inline_gadget(
						wide, condition)) &&
					words[1] == (register_operand(3, 4) | (2ULL << 48)) &&
					words[2] == cells.word(at(1, displacement)),
				"linked CMP register + B.cond inline fields changed");
		}
}

void a_full_suffix_table_ends_the_block_at_a_fused_pair(
	std::int64_t displacement)
{
	for (const bool wide : {false, true})
		for (unsigned condition = 0; condition < selectable_conditions;
		     ++condition)
			for (const bool immediate : {false, true}) {
				auto block = full_table();
				block.push_back(immediate ?
					compare_immediate(wide, 3, 5, false) :
					compare_register(wide, 3, 4));
				block.push_back(encode_conditional_branch(
					false, condition, displacement));
				block.push_back(nop);
				Cells cells;
				const auto built = build(block, &cells);
				const auto words = built.program.words();
				require(built.complete &&
						built.program.instruction_count() ==
							inline_suffix_slots + 2U &&
						words.size() == full_table_words + 4U,
					"the fused pair did not end the block");
				require(words[full_table_words] == entry(immediate ?
						selection::compare_immediate_branch_gadget(
							wide, condition, true) :
						selection::compare_register_branch_gadget(
							wide, condition, true)) &&
						words[full_table_words + 1] == (immediate ?
							immediate_operand(3, 5) :
							register_operand(3, 4)) &&
						words[full_table_words + 2] == cells.word(at(
							inline_suffix_slots + 1, displacement)) &&
						words[full_table_words + 3] == cells.word(
							at(inline_suffix_slots + 2)),
					"the terminal fused pair fields changed");
			}
}

void linked_compare_branches(std::int64_t displacement)
{
	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true}) {
			Cells cells;
			auto built = build({encode_compare_branch(
				wide, nonzero, displacement, 7), nop, nop}, &cells);
			auto words = built.program.words();
			require(built.complete && built.program.instruction_count() == 3U,
				"linked compare branch did not translate the block");
			require(words[0] == entry(selection::compare_branch_inline_gadget(
					wide, nonzero)) &&
					words[1] == (register_offset(7) | (2ULL << 16)) &&
					words[2] == cells.word(at(0, displacement)),
				"linked compare branch inline fields changed");

			auto block = full_table();
			block.push_back(encode_compare_branch(
				wide, nonzero, displacement, 7));
			built = build(block, &cells);
			words = built.program.words();
			require(built.complete && words.size() == full_table_words + 4U &&
					built.program.instruction_count() ==
						inline_suffix_slots + 1U,
				"a full table did not end the block at a compare branch");
			require(words[full_table_words] == entry(selection::
					compare_branch_gadget(wide, nonzero, true)) &&
					words[full_table_words + 1] == register_offset(7) &&
					words[full_table_words + 2] == cells.word(at(
						inline_suffix_slots, displacement)) &&
					words[full_table_words + 3] == cells.word(
						at(inline_suffix_slots + 1)),
				"the terminal compare branch fields changed");
		}
}

void linked_test_branches(std::int64_t displacement)
{
	for (const bool nonzero : {false, true})
		for (const unsigned bit : {0U, 31U, 32U, 63U}) {
			Cells cells;
			auto built = build({encode_test_branch(
				nonzero, bit, displacement, 11), nop, nop}, &cells);
			auto words = built.program.words();
			require(built.complete && built.program.instruction_count() == 3U,
				"linked test branch did not translate the block");
			require(words[0] == entry(selection::test_bit_branch_inline_gadget(
					nonzero)) &&
					words[1] == (bit | (register_offset(11) << 16) |
						(2ULL << 32)) &&
					words[2] == cells.word(at(0, displacement)),
				"linked test branch inline fields changed");

			auto block = full_table();
			block.push_back(encode_test_branch(
				nonzero, bit, displacement, 11));
			built = build(block, &cells);
			words = built.program.words();
			require(built.complete && words.size() == full_table_words + 4U,
				"a full table did not end the block at a test branch");
			require(words[full_table_words] == entry(selection::
					test_bit_branch_gadget(nonzero, true)) &&
					words[full_table_words + 1] ==
						(bit | (register_offset(11) << 16)) &&
					words[full_table_words + 2] == cells.word(at(
						inline_suffix_slots, displacement)) &&
					words[full_table_words + 3] == cells.word(
						at(inline_suffix_slots + 1)),
				"the terminal test branch fields changed");
		}
}

void linked_conditional_branches(std::int64_t displacement)
{
	for (unsigned condition = 0; condition < 16; ++condition) {
		const auto branch = encode_conditional_branch(
			false, condition, displacement);
		Cells cells;
		auto built = build({branch, nop, nop}, &cells);
		auto words = built.program.words();
		if (condition < selectable_conditions) {
			require(built.complete && built.program.instruction_count() == 3U,
				"linked B.cond did not translate the block");
			require(words[0] == entry(selection::
					conditional_branch_inline_gadget(condition)) &&
					words[1] == cells.word(at(0, displacement)) &&
					words[2] == 2U,
				"linked B.cond inline fields changed");
		} else {
			require(built.complete && built.program.instruction_count() == 1U &&
					words.size() == 3U,
				"linked B.AL did not end the block");
			require(words[0] == entry(selection::
					conditional_branch_linked_gadget(condition)) &&
					words[1] == cells.word(at(0, displacement)) &&
					words[2] == cells.word(at(1)),
				"linked B.AL fields changed");
		}

		auto block = full_table();
		block.push_back(branch);
		built = build(block, &cells);
		words = built.program.words();
		require(built.complete && words.size() == full_table_words + 3U &&
				built.program.instruction_count() == inline_suffix_slots + 1U,
			"a full table did not end the block at B.cond");
		require(words[full_table_words] == entry(selection::
				conditional_branch_linked_gadget(condition)) &&
				words[full_table_words + 1] == cells.word(
					at(inline_suffix_slots, displacement)) &&
				words[full_table_words + 2] == cells.word(
					at(inline_suffix_slots + 1)),
			"the terminal B.cond fields changed");
	}
}

void branches_on_xzr_are_unconditional(
	std::int64_t displacement, std::int64_t test_displacement)
{
	/* XZR is zero: CBZ and TBZ always branch, CBNZ and TBNZ never. */
	for (const bool nonzero : {false, true})
		for (const auto &[instruction, selected] : {
			     std::pair{encode_compare_branch(
				     true, nonzero, displacement, 31),
				     nonzero ? at(1) : at(0, displacement)},
			     std::pair{encode_test_branch(
				     nonzero, 63, test_displacement, 31),
				     nonzero ? at(1) : at(0, test_displacement)}}) {
			Cells cells;
			const auto built = build({instruction, nop}, &cells);
			const auto words = built.program.words();
			require(built.complete && words.size() == 2U &&
					built.program.instruction_count() == 1U &&
					words[0] == reinterpret_cast<std::uintptr_t>(
						uml_arm64_gadget_branch_always_linked) &&
					words[1] == cells.word(selected),
				"a linked branch on XZR was not one unconditional link");
			const auto unlinked = build({instruction, nop}, nullptr);
			require(unlinked.program.words().size() == 2U &&
					unlinked.program.words()[0] ==
						reinterpret_cast<std::uintptr_t>(
							uml_arm64_gadget_branch_always) &&
					unlinked.program.words()[1] == selected,
				"an unlinked branch on XZR was not one unconditional address");
		}
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	using namespace uml::emulator::tests::base_control;
	for (std::size_t i = 0; i < long_displacements.size(); ++i) {
		const auto displacement = long_displacements[i];
		unlinked_fused_pairs(displacement);
		linked_fused_pairs_inline_their_fall_through(displacement);
		a_full_suffix_table_ends_the_block_at_a_fused_pair(displacement);
		linked_compare_branches(displacement);
		linked_conditional_branches(displacement);
		linked_test_branches(short_displacements[i]);
		branches_on_xzr_are_unconditional(displacement,
			short_displacements[i]);
	}
	return 0;
}
