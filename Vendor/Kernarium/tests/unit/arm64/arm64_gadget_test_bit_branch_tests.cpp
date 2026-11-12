// SPDX-License-Identifier: GPL-2.0-or-later
/* TBZ and TBNZ name the bit they test in their operand word instead of a mask
 * word. Every bit of the 64-bit register, both polarities, several registers
 * and values, and XZR, run in each shape the builder emits: ending the block
 * with the guest PCs, ending it through links, translated inline with its
 * fall-through behind it, and ending it through links after the block's
 * inline suffixes ran out. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" {
void uml_arm64_gadget_link_exit();
void uml_arm64_gadget_test_bit_branch_z();
void uml_arm64_gadget_test_bit_branch_nz();
void uml_arm64_gadget_test_bit_branch_z_linked();
void uml_arm64_gadget_test_bit_branch_nz_linked();
void uml_arm64_gadget_test_bit_branch_z_inline();
void uml_arm64_gadget_test_bit_branch_nz_inline();
}

namespace {
using uml::emulator::tests::address;
using uml::testing::require;

constexpr std::uint64_t base = 0x4000;
constexpr std::uint64_t taken_pc = 0x5000;
constexpr std::uint32_t svc = 0xd400'0001U;

/* The block a link names: x19 = marker, then SVC. */
gadgets::BuildResult target_block(std::uint64_t pc, std::uint16_t marker)
{
	const std::array instructions{
		0xd280'0013U | (static_cast<std::uint32_t>(marker) << 5), svc};
	return gadgets::Builder{}.build(pc, instructions);
}

struct LinkTarget final {
	explicit LinkTarget(std::uint64_t pc) noexcept
		: fallback_stream{address(uml_arm64_gadget_link_exit)},
		  link{.stream = &fallback_stream, .instruction_count = 0,
			.guest_pc = pc}
	{
	}
	LinkTarget(const LinkTarget &) = delete;
	LinkTarget &operator=(const LinkTarget &) = delete;

	void resolve(const gadgets::BuildResult &built) noexcept
	{
		link.stream = built.program.words().data();
		link.instruction_count = built.program.instruction_count();
	}

	std::uintptr_t fallback_stream;
	gadgets::BlockLink link;
};

std::uint32_t test_bit(std::uint64_t pc, std::uint64_t target, bool nonzero,
	unsigned bit, unsigned reg)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x3fffU);
	return 0x3600'0000U | (static_cast<std::uint32_t>(bit >> 5) << 31) |
		(static_cast<std::uint32_t>(bit & 31U) << 19) |
		(static_cast<std::uint32_t>(nonzero) << 24) | (immediate << 5) | reg;
}

std::uint64_t next_random(std::uint64_t &seed)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

enum class Shape { unlinked, linked_inline, linked_terminal };

struct Outcome final {
	bool taken;
	std::size_t executed;
	std::uint64_t pc;
	::uml::cpu::EngineEvent event;
};

/* Runs one TBZ/TBNZ in the shape and reports where it went. Linked shapes
 * land in a block that sets x19 to 1 (taken) or 2 (not taken). */
Outcome run(Shape shape, bool nonzero, unsigned bit, unsigned reg,
	std::uint64_t value, std::size_t &stream_words)
{
	auto taken_block = target_block(taken_pc, 1);
	auto fall_block = target_block(base + 4, 2);
	LinkTarget taken_link{taken_pc};
	LinkTarget fall_link{base + 4};
	taken_link.resolve(taken_block);
	fall_link.resolve(fall_block);

	std::vector<std::uint32_t> code;
	/* 64 never-taken inline branches use up the block's suffix slots, so the
	 * test branch after them ends the block through its links. */
	std::size_t padding = 0;
	if (shape == Shape::linked_terminal) {
		padding = gadgets::InlineSuffixes::capacity;
		for (std::size_t i = 0; i < padding; ++i) {
			/* CBNZ X20, +8: X20 is zero, so it falls through. */
			code.push_back(0xb500'0054U);
		}
	}
	const auto at = base + code.size() * 4;
	code.push_back(test_bit(at, taken_pc, nonzero, bit, reg));
	/* A64 TBZ displacement is relative to the branch itself. */
	std::vector<std::uint32_t> program_code = code;

	gadgets::Builder::LinkResolver resolver;
	if (shape != Shape::unlinked) {
		resolver = [&](std::uint64_t pc) {
			return pc == taken_pc ? &taken_link.link : &fall_link.link;
		};
		if (shape == Shape::linked_terminal)
			fall_link.link.guest_pc = at + 4;
	}
	/* The fall-through of a block that ends here is the next address. */
	const auto built = gadgets::Builder{}.build(base, program_code, resolver);
	require(built.complete);
	stream_words = built.program.words().size();
	/* The shape asked for is the shape built. */
	const auto words = built.program.words();
	const auto has = [&](void (*gadget)()) {
		return std::find(words.begin(), words.end(), address(gadget)) !=
			words.end();
	};
	switch (shape) {
	case Shape::unlinked:
		require(has(nonzero ? uml_arm64_gadget_test_bit_branch_nz :
			uml_arm64_gadget_test_bit_branch_z) || reg == 31U);
		break;
	case Shape::linked_inline:
		require(has(nonzero ? uml_arm64_gadget_test_bit_branch_nz_inline :
			uml_arm64_gadget_test_bit_branch_z_inline) || reg == 31U);
		break;
	case Shape::linked_terminal:
		require(has(nonzero ? uml_arm64_gadget_test_bit_branch_nz_linked :
			uml_arm64_gadget_test_bit_branch_z_linked) || reg == 31U);
		break;
	}

	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[reg == 31U ? 0 : reg] = value;
	cpu.registers[20] = 0;
	::uml::memory::Tlb tlb;
	std::size_t executed = 0;
	const auto event = gadgets::execute(built.program, cpu, tlb, executed);
	Outcome outcome{};
	outcome.executed = executed;
	outcome.pc = cpu.pc;
	outcome.event = event;
	if (shape == Shape::unlinked) {
		outcome.taken = cpu.pc == taken_pc;
		require(cpu.pc == taken_pc || cpu.pc == at + 4);
	} else {
		require(event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[19] == 1 || cpu.registers[19] == 2);
		outcome.taken = cpu.registers[19] == 1;
	}
	return outcome;
}

} // namespace

int main()
{
	std::uint64_t seed = 0x5d1b'93a7'0c4e'f286ULL;
	std::size_t runs = 0;
	const std::array<unsigned, 4> registers{0, 7, 30, 31};
	for (const auto shape : {Shape::unlinked, Shape::linked_inline,
		     Shape::linked_terminal})
	for (const bool nonzero : {false, true})
	for (unsigned bit = 0; bit < 64; ++bit)
	for (const auto reg : registers) {
		const std::array<std::uint64_t, 4> values{
			next_random(seed) | (std::uint64_t{1} << bit),
			next_random(seed) & ~(std::uint64_t{1} << bit),
			~std::uint64_t{0}, 0};
		for (const auto value : values) {
			std::size_t words = 0;
			const auto outcome = run(shape, nonzero, bit, reg, value, words);
			const bool set = reg != 31U && ((value >> bit) & 1U) != 0;
			/* TBZ is taken when the bit is clear, TBNZ when it is set. */
			require(outcome.taken == (nonzero ? set : !set));
			/* Retired: the padding, the branch and, for the linked shapes,
			 * the target block's MOVZ and SVC. */
			const std::size_t padding =
				shape == Shape::linked_terminal ?
				gadgets::InlineSuffixes::capacity : 0;
			if (shape == Shape::unlinked)
				require(outcome.executed == 1 &&
					outcome.event == ::uml::cpu::EngineEvent::continued);
			else
				require(outcome.executed == padding + 3);
			++runs;
		}
	}
	/* The stream: an ordinary TBZ takes the gadget and one operand word, with
	 * the bit in bits 5:0 and the register's CpuState offset in 31:16. */
	{
		const std::array code{test_bit(base, taken_pc, false, 37, 7)};
		const auto built = gadgets::Builder{}.build(base, code);
		const auto words = built.program.words();
		require(words[0] == address(uml_arm64_gadget_test_bit_branch_z));
		require(words[1] == (37U | (offsetof(arm64::CpuState, registers) +
			7U * 8U) << 16));
		require(words[2] == taken_pc && words[3] == base + 4);
	}
	std::printf("test bit branch: %zu runs\n", runs);
	std::puts("gadget test bit branch tests passed");
	return 0;
}
