// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"

#include <cstdint>
#include <cstring>
#include <memory>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

using Memory = std::unique_ptr<StaticGuestMemory>;

/* CBZ Xn, +offset */
[[nodiscard]] constexpr std::uint32_t compare_branch_zero(
	unsigned reg, int words) noexcept
{
	return 0xb400'0000U |
		((static_cast<std::uint32_t>(words) & 0x7ffffU) << 5U) |
		(reg & 31U);
}

/* B.EQ +offset */
[[nodiscard]] constexpr std::uint32_t branch_equal(int words) noexcept
{
	return 0x5400'0000U |
		((static_cast<std::uint32_t>(words) & 0x7ffffU) << 5U);
}

/* TBZ Xn, #0, +offset */
[[nodiscard]] constexpr std::uint32_t test_bit_zero(
	unsigned reg, int words) noexcept
{
	return 0x3600'0000U |
		((static_cast<std::uint32_t>(words) & 0x3fffU) << 5U) |
		(reg & 31U);
}

[[nodiscard]] constexpr std::uint32_t compare_immediate_zero(
	unsigned reg) noexcept
{
	/* SUBS XZR, Xn, #0 (CMP Xn, #0). */
	return 0xf100'001fU | ((reg & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t compare_registers(
	unsigned left, unsigned right) noexcept
{
	/* SUBS XZR, Xn, Xm (CMP Xn, Xm). */
	return 0xeb00'001fU | ((right & 31U) << 16U) |
		((left & 31U) << 5U);
}

/* A block whose conditional branch is followed by more translated
 * instructions. Taking the branch leaves from the middle of the block, which
 * before the fall-through was inlined could not happen: every exit was a
 * block's last instruction. */
void write_block(JitBacking &backing, unsigned taken)
{
	/* 0: mov x1, #1        -- retires
	 * 4: cbz x0, +5 words  -- retires, and leaves if x0 == 0
	 * 8: mov x2, #2        -- only on the fall-through
	 * c: mov x3, #3
	 * 10: mov x4, #4
	 * 14: svc
	 * 18: (branch target) mov x5, #5
	 * 1c: svc
	 */
	backing.put32(0x00, move_immediate(1, 1));
	backing.put32(0x04, compare_branch_zero(0, taken));
	backing.put32(0x08, move_immediate(2, 2));
	backing.put32(0x0c, move_immediate(3, 3));
	backing.put32(0x10, move_immediate(4, 4));
	backing.put32(0x14, system_call);
	backing.put32(0x18, move_immediate(5, 5));
	backing.put32(0x1c, system_call);
}

void write_nonpacked_branch_block(
	JitBacking &backing, std::uint32_t branch)
{
	backing.put32(0x00, move_immediate(1, 1));
	backing.put32(0x04, branch);
	backing.put32(0x08, move_immediate(2, 2));
	backing.put32(0x0c, move_immediate(3, 3));
	backing.put32(0x10, move_immediate(4, 4));
	backing.put32(0x14, system_call);
	backing.put32(0x18, move_immediate(5, 5));
	backing.put32(0x1c, system_call);
}

void write_fused_compare_branch_block(
	JitBacking &backing, std::uint32_t compare)
{
	backing.put32(0x00, compare);
	backing.put32(0x04, branch_equal(5));
	backing.put32(0x08, move_immediate(2, 2));
	backing.put32(0x0c, move_immediate(3, 3));
	backing.put32(0x10, move_immediate(4, 4));
	backing.put32(0x14, system_call);
	backing.put32(0x18, move_immediate(5, 5));
	backing.put32(0x1c, system_call);
}

/* The fall-through path retires every instruction the block translated. */
void the_fall_through_retires_the_whole_block()
{
	constexpr unsigned long generation = 940;
	JitBacking backing{1};
	write_block(backing, 5);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 1; /* nonzero: the branch is not taken */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	require(outcome.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[1] == 1);
	require(cpu.registers[2] == 2);
	require(cpu.registers[3] == 3);
	require(cpu.registers[4] == 4);
	/* mov, cbz, mov, mov, mov, svc */
	require(outcome.executed == 6);
}

/* Taking the branch retires only what ran before it.
 *
 * This is the count the change had to keep honest. Entering a block precharges
 * its whole translated length, and with the fall-through inlined the branch is
 * no longer the last instruction, so the taken path has to give the rest back.
 * Getting this wrong is invisible to every conformance test -- the guest's
 * registers are identical either way -- and shows up only as a wrong retirement
 * count.
 */
void taking_the_branch_retires_only_what_ran()
{
	constexpr unsigned long generation = 941;
	JitBacking backing{1};
	write_block(backing, 5);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 0; /* zero: the branch is taken */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	/* The target has not been translated yet, so its link is still the
	 * unlinked exit and the branch leaves here. */
	require(outcome.event == ::uml::cpu::EngineEvent::continued);
	require(cpu.pc == code_base + 0x18);
	require(cpu.registers[1] == 1);
	/* The instructions translated behind the branch must not have run. */
	require(cpu.registers[2] == 0);
	require(cpu.registers[3] == 0);
	require(cpu.registers[4] == 0);
	/* The block precharged all six instructions it translated. Two ran, so
	 * the taken path had to give four back. */
	require(outcome.executed == 2);

	const auto target = run_once(*executor, *memory, generation, cpu);
	require(target.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[5] == 5);
	require(target.executed == 2);
}

void ordinary_inline_suffix_packing_preserves_branch_behavior()
{
	unsigned long generation = 946;
	for (const auto branch : {branch_equal(5), test_bit_zero(0, 5)}) {
		JitBacking backing{1};
		write_nonpacked_branch_block(backing, branch);
		JitMapping mapping{generation, code_base, backing, 1};

		Memory memory{std::make_unique<StaticGuestMemory>()};
		auto executor = std::make_unique<gadgets::GadgetExecutor>();
		arm64::CpuState cpu{};
		cpu.pc = code_base;
		cpu.nzcv = 1U << 30; /* B.EQ is taken; TBZ ignores flags. */
		cpu.registers[0] = 0; /* TBZ is taken. */
		const auto outcome = run_once(
			*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued);
		require(cpu.pc == code_base + 0x18);
		require(outcome.executed == 2);
		++generation;
	}
}

void fused_compare_inline_suffix_packing_preserves_branch_behavior()
{
	unsigned long generation = 948;
	for (const auto compare : {
		     compare_immediate_zero(0), compare_registers(0, 1)}) {
		JitBacking backing{1};
		write_fused_compare_branch_block(backing, compare);
		JitMapping mapping{generation, code_base, backing, 1};

		Memory memory{std::make_unique<StaticGuestMemory>()};
		auto executor = std::make_unique<gadgets::GadgetExecutor>();
		arm64::CpuState cpu{};
		cpu.pc = code_base;
		cpu.registers[0] = 7;
		cpu.registers[1] = 7;
		if (compare == compare_immediate_zero(0))
			cpu.registers[0] = 0;
		const auto outcome = run_once(
			*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued);
		require(cpu.pc == code_base + 0x18);
		require(outcome.executed == 2);
		++generation;
	}
}

/* A fault after the branch still lands on the right instruction.
 *
 * Fault correction converts a faulting PC to its position with
 * (pc - base) / 4, which stays exact only because a fall-through is the next
 * address. This runs into a translated instruction past the branch and
 * requires the retirement count to match the address it stopped at.
 */
void a_fault_past_the_branch_counts_from_its_address()
{
	constexpr unsigned long generation = 942;
	JitBacking backing{1};
	write_block(backing, 5);
	/* Replace the third instruction after the branch with an undefined
	 * encoding, so the block translates but stops there when executed. */
	backing.put32(0x10, 0x0000'0000U);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 1; /* not taken: fall through into the fault */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	require(outcome.event ==
		::uml::cpu::EngineEvent::illegal_instruction);
	require(cpu.pc == code_base + 0x10);
	/* mov, cbz, mov, mov retired; the undefined word did not. */
	require(outcome.executed == 4);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	the_fall_through_retires_the_whole_block();
	taking_the_branch_retires_only_what_ran();
	ordinary_inline_suffix_packing_preserves_branch_behavior();
	fused_compare_inline_suffix_packing_preserves_branch_behavior();
	a_fault_past_the_branch_counts_from_its_address();
	return 0;
}
