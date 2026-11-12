// SPDX-License-Identifier: GPL-2.0-or-later
/* Every memory transfer of the gadget engine names its instruction by its
 * position in the block, carried in the top byte of its operand, instead of
 * a PC word. A fault reports that position; the executor turns it back into
 * the PC of the instruction. This test faults one instruction of each
 * family, at several positions in a block, directly and through a link, with
 * an unmapped page, a page crossing, a misaligned single-copy-atomic access
 * and a misaligned stack pointer, and requires the PC, the retired count and
 * the untouched architectural state of the plain run. */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>
#include <sys/mman.h>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::cpu::arm64::gadgets::GadgetExecutor;
using uml::memory::StaticGuestMemory;
using namespace uml::emulator::tests;

namespace {

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t code_base = 0x90'0000;
/* One block per slot: sixty-two pads, the transfer, a MOVZ and an SVC. */
constexpr std::uint64_t slot = 0x180;
constexpr std::uint32_t pad = 0x9100'0694U;      // add x20, x20, #1
constexpr std::uint32_t after = 0xd280'00e9U;    // movz x9, #7
constexpr std::uint32_t svc = 0xd400'0001U;      // svc #0
constexpr std::array<unsigned, 3> positions{0, 1, 62};
/* A direct block per position and a branch to the first and last. */
constexpr unsigned slots_per_case = 5;

struct Case final {
	std::uint32_t word;       // 0 for a literal load, built per slot
	unsigned size;            // bytes the instruction transfers
	int displacement;         // from the base register, before writeback
	bool single_copy;         // exclusive, atomic or ordered: faults when
	                          // misaligned across a 16-byte block
	bool stack_base;          // the base register is SP
	bool stack_checked;       // SP must be 16-byte aligned for it
	const char *text;
};

/* Assembled forms of every family that carries a position: scalar and vector
 * register offset, unscaled, pre- and post-index; vector unsigned offset and
 * pairs; atomics, CAS, CASP; exclusives; ordered and RCPC; SIMD structures,
 * lanes and replicates. */
constexpr Case cases[] = {
	{0xf863'6841U, 8, 0, false, false, false, "ldr x1, [x2, x3]"},
	{0xf863'7841U, 8, 0, false, false, false, "ldr x1, [x2, x3, lsl #3]"},
	{0xb863'4841U, 4, 0, false, false, false, "ldr w1, [x2, w3, uxtw]"},
	{0xb863'd841U, 4, 0, false, false, false, "ldr w1, [x2, w3, sxtw #2]"},
	{0x3863'6841U, 1, 0, false, false, false, "ldrb w1, [x2, x3]"},
	{0x7863'6841U, 2, 0, false, false, false, "ldrh w1, [x2, x3]"},
	{0x38a3'6841U, 1, 0, false, false, false, "ldrsb x1, [x2, x3]"},
	{0xf823'6841U, 8, 0, false, false, false, "str x1, [x2, x3]"},
	{0x3823'6841U, 1, 0, false, false, false, "strb w1, [x2, x3]"},
	{0x7823'6841U, 2, 0, false, false, false, "strh w1, [x2, x3]"},
	{0xb823'6841U, 4, 0, false, false, false, "str w1, [x2, x3]"},
	{0xf850'0041U, 8, -256, false, false, false, "ldur x1, [x2, #-0x100]"},
	{0xf84f'f041U, 8, 255, false, false, false, "ldur x1, [x2, #0xff]"},
	{0xf840'8c41U, 8, 8, false, false, false, "ldr x1, [x2, #0x8]!"},
	{0xf85f'8441U, 8, 0, false, false, false, "ldr x1, [x2], #-0x8"},
	{0xf81f'0c41U, 8, -16, false, false, false, "str x1, [x2, #-0x10]!"},
	{0xf801'0441U, 8, 0, false, false, false, "str x1, [x2], #0x10"},
	{0x3840'1c41U, 1, 1, false, false, false, "ldrb w1, [x2, #0x1]!"},
	{0x7880'2441U, 2, 0, false, false, false, "ldrsh x1, [x2], #0x2"},
	{0xb840'4441U, 4, 0, false, false, false, "ldr w1, [x2], #0x4"},
	{0x3800'1441U, 1, 0, false, false, false, "strb w1, [x2], #0x1"},
	{0xb800'3041U, 4, 3, false, false, false, "stur w1, [x2, #0x3]"},
	{0x3dc0'0041U, 16, 0, false, false, false, "ldr q1, [x2]"},
	{0x3dc3'fc41U, 16, 4080, false, false, false, "ldr q1, [x2, #0xff0]"},
	{0xfd40'0441U, 8, 8, false, false, false, "ldr d1, [x2, #0x8]"},
	{0xbd40'0441U, 4, 4, false, false, false, "ldr s1, [x2, #0x4]"},
	{0x7d40'0441U, 2, 2, false, false, false, "ldr h1, [x2, #0x2]"},
	{0x3d40'0441U, 1, 1, false, false, false, "ldr b1, [x2, #0x1]"},
	{0x3d80'0041U, 16, 0, false, false, false, "str q1, [x2]"},
	{0xfd00'0841U, 8, 16, false, false, false, "str d1, [x2, #0x10]"},
	{0xbd00'0041U, 4, 0, false, false, false, "str s1, [x2]"},
	{0x7d00'0041U, 2, 0, false, false, false, "str h1, [x2]"},
	{0x3d00'0041U, 1, 0, false, false, false, "str b1, [x2]"},
	{0x3ce3'6841U, 16, 0, false, false, false, "ldr q1, [x2, x3]"},
	{0x3ce3'7841U, 16, 0, false, false, false, "ldr q1, [x2, x3, lsl #4]"},
	{0xfc63'd841U, 8, 0, false, false, false, "ldr d1, [x2, w3, sxtw #3]"},
	{0x3ca3'6841U, 16, 0, false, false, false, "str q1, [x2, x3]"},
	{0xbc23'5841U, 4, 0, false, false, false, "str s1, [x2, w3, uxtw #2]"},
	{0x3cd0'0041U, 16, -256, false, false, false, "ldur q1, [x2, #-0x100]"},
	{0x3cc1'0c41U, 16, 16, false, false, false, "ldr q1, [x2, #0x10]!"},
	{0x3cdf'0441U, 16, 0, false, false, false, "ldr q1, [x2], #-0x10"},
	{0xfc1f'8c41U, 8, -8, false, false, false, "str d1, [x2, #-0x8]!"},
	{0x3c8f'0441U, 16, 0, false, false, false, "str q1, [x2], #0xf0"},
	{0xad40'0440U, 32, 0, false, false, false, "ldp q0, q1, [x2]"},
	{0xad60'0440U, 32, -1024, false, false, false, "ldp q0, q1, [x2, #-0x400]"},
	{0xad5f'8440U, 32, 1008, false, false, false, "ldp q0, q1, [x2, #0x3f0]"},
	{0xad00'0440U, 32, 0, false, false, false, "stp q0, q1, [x2]"},
	{0x6de0'0440U, 16, -512, false, false, false, "ldp d0, d1, [x2, #-0x200]!"},
	{0x6c9f'8440U, 16, 0, false, false, false, "stp d0, d1, [x2], #0x1f8"},
	{0x2de0'0440U, 8, -256, false, false, false, "ldp s0, s1, [x2, #-0x100]!"},
	{0x2c9f'8440U, 8, 0, false, false, false, "stp s0, s1, [x2], #0xfc"},
	{0xf821'0043U, 8, 0, true, false, false, "ldadd x1, x3, [x2]"},
	{0xb8e1'0043U, 4, 0, true, false, false, "ldaddal w1, w3, [x2]"},
	{0x3821'3043U, 1, 0, true, false, false, "ldsetb w1, w3, [x2]"},
	{0xf821'8043U, 8, 0, true, false, false, "swp x1, x3, [x2]"},
	{0x7821'105fU, 2, 0, true, false, false, "stclrh w1, [x2]"},
	{0xc8a1'7c43U, 8, 0, true, false, false, "cas x1, x3, [x2]"},
	{0x88e1'fc43U, 4, 0, true, false, false, "casal w1, w3, [x2]"},
	{0x08a1'7c43U, 1, 0, true, false, false, "casb w1, w3, [x2]"},
	{0x4820'7c44U, 16, 0, true, false, false, "casp x0, x1, x4, x5, [x2]"},
	{0x0860'fc44U, 8, 0, true, false, false, "caspal w0, w1, w4, w5, [x2]"},
	{0xc85f'7c41U, 8, 0, true, false, false, "ldxr x1, [x2]"},
	{0x885f'fc41U, 4, 0, true, false, false, "ldaxr w1, [x2]"},
	{0x085f'7c41U, 1, 0, true, false, false, "ldxrb w1, [x2]"},
	{0xc803'7c41U, 8, 0, true, false, false, "stxr w3, x1, [x2]"},
	{0x8803'fc41U, 4, 0, true, false, false, "stlxr w3, w1, [x2]"},
	{0x4803'7c41U, 2, 0, true, false, false, "stxrh w3, w1, [x2]"},
	{0xc87f'0c41U, 16, 0, true, false, false, "ldxp x1, x3, [x2]"},
	{0x887f'8c41U, 8, 0, true, false, false, "ldaxp w1, w3, [x2]"},
	{0xc824'0c41U, 16, 0, true, false, false, "stxp w4, x1, x3, [x2]"},
	{0xc83d'f85cU, 16, 0, true, false, false, "stlxp w29, x28, x30, [x2]"},
	{0xc83e'73fdU, 16, 0, true, true, true, "stxp w30, x29, x28, [sp]"},
	{0xc8df'fc41U, 8, 0, true, false, false, "ldar x1, [x2]"},
	{0x08df'fc41U, 1, 0, true, false, false, "ldarb w1, [x2]"},
	{0x48df'fc41U, 2, 0, true, false, false, "ldarh w1, [x2]"},
	{0xc89f'fc41U, 8, 0, true, false, false, "stlr x1, [x2]"},
	{0x089f'fc41U, 1, 0, true, false, false, "stlrb w1, [x2]"},
	{0xf8bf'c041U, 8, 0, false, false, false, "ldapr x1, [x2]"},
	{0x38bf'c041U, 1, 0, false, false, false, "ldaprb w1, [x2]"},
	{0xd950'0041U, 8, -256, false, false, false, "ldapur x1, [x2, #-0x100]"},
	{0x994f'f041U, 4, 255, false, false, false, "ldapur w1, [x2, #0xff]"},
	{0x1980'5041U, 1, 5, false, false, false, "ldapursb x1, [x2, #0x5]"},
	{0xd91f'8041U, 8, -8, false, false, false, "stlur x1, [x2, #-0x8]"},
	{0x1900'1041U, 1, 1, false, false, false, "stlurb w1, [x2, #0x1]"},
	{0x4c40'7040U, 16, 0, false, false, false, "ld1.16b { v0 }, [x2]"},
	{0x4c40'2c40U, 64, 0, false, false, false, "ld1.2d { v0, v1, v2, v3 }, [x2]"},
	{0x4c40'8840U, 32, 0, false, false, false, "ld2.4s { v0, v1 }, [x2]"},
	{0x0c40'4040U, 24, 0, false, false, false, "ld3.8b { v0, v1, v2 }, [x2]"},
	{0x4cdf'0440U, 64, 0, false, false, false, "ld4.8h { v0, v1, v2, v3 }, [x2], #64"},
	{0x4cc3'7040U, 16, 0, false, false, false, "ld1.16b { v0 }, [x2], x3"},
	{0x4c00'7040U, 16, 0, false, false, false, "st1.16b { v0 }, [x2]"},
	{0x4c9f'8840U, 32, 0, false, false, false, "st2.4s { v0, v1 }, [x2], #32"},
	{0x4c83'0c5cU, 64, 0, false, false, false, "st4.2d { v28, v29, v30, v31 }, [x2], x3"},
	{0x4d40'1c40U, 1, 0, false, false, false, "ld1.b { v0 }[15], [x2]"},
	{0x4ddf'5840U, 2, 0, false, false, false, "ld1.h { v0 }[7], [x2], #2"},
	{0x4d60'9040U, 8, 0, false, false, false, "ld2.s { v0, v1 }[3], [x2]"},
	{0x4dc3'a440U, 24, 0, false, false, false, "ld3.d { v0, v1, v2 }[1], [x2], x3"},
	{0x4d00'8440U, 8, 0, false, false, false, "st1.d { v0 }[1], [x2]"},
	{0x4dbf'a040U, 16, 0, false, false, false, "st4.s { v0, v1, v2, v3 }[2], [x2], #16"},
	{0x4d40'c840U, 4, 0, false, false, false, "ld1r.4s { v0 }, [x2]"},
	{0x4ddf'e040U, 3, 0, false, false, false, "ld3r.16b { v0, v1, v2 }, [x2], #3"},
	{0x4de3'ec40U, 32, 0, false, false, false, "ld4r.2d { v0, v1, v2, v3 }, [x2], x3"},
	{0xf940'03e1U, 8, 0, false, true, false, "ldr x1, [sp]"},
	{0xadff'07e0U, 32, -32, false, true, false, "ldp q0, q1, [sp, #-0x20]!"},
	{0xb81f'0fe1U, 4, -16, false, true, false, "str w1, [sp, #-0x10]!"},
	{0xc803'7fe1U, 8, 0, true, true, true, "stxr w3, x1, [sp]"},
	{0xf8e1'03e3U, 8, 0, true, true, true, "ldaddal x1, x3, [sp]"},
	{0x4c40'73e0U, 16, 0, false, true, false, "ld1.16b { v0 }, [sp]"},
	{0xc85f'7fe1U, 8, 0, true, true, true, "ldxr x1, [sp]"},
	{0xc87f'8fe1U, 16, 0, true, true, true, "ldaxp x1, x3, [sp]"},
	{0xc8a1'7fe3U, 8, 0, true, true, true, "cas x1, x3, [sp]"},
	{0x4820'7fe4U, 16, 0, true, true, true, "casp x0, x1, x4, x5, [sp]"},
	{0xf821'83e3U, 8, 0, true, true, true, "swp x1, x3, [sp]"},
	{0xc83d'fbfcU, 16, 0, true, true, true, "stlxp w29, x28, x30, [sp]"},
	{0x0d40'0fe0U, 1, 0, false, true, false, "ld1.b { v0 }[3], [sp]"},
	{0x4d40'cbe0U, 4, 0, false, true, false, "ld1r.4s { v0 }, [sp]"},
	{0x4c00'8be0U, 32, 0, false, true, false, "st2.4s { v0, v1 }, [sp]"},
	{0x4cdf'73e0U, 16, 0, false, true, false, "ld1.16b { v0 }, [sp], #16"},
	{0x4c40'07e0U, 64, 0, false, true, false, "ld4.8h { v0, v1, v2, v3 }, [sp]"},
};

/* Literal loads: ldr w, ldr x, ldrsw, ldr s, ldr d, ldr q. */
constexpr std::array<std::uint32_t, 6> literal_templates{
	0x1800'0001U, 0x5800'0001U, 0x9800'0001U,
	0x1c00'0001U, 0x5c00'0001U, 0x9c00'0001U,
};
constexpr const char *literal_text[]{
	"ldr w1, literal", "ldr x1, literal", "ldrsw x1, literal",
	"ldr s1, literal", "ldr d1, literal", "ldr q1, literal"};

constexpr std::size_t case_count =
	std::size(cases) + literal_templates.size();
constexpr std::uint64_t code_bytes =
	case_count * slots_per_case * slot;
constexpr std::uint64_t code_pages = (code_bytes + page - 1) / page;
constexpr std::uint64_t data_pages = 2;
constexpr std::uint64_t data_base = code_base + code_pages * page;
constexpr std::uint64_t data_end = data_base + data_pages * page;
/* The page after the data is unmapped, and so is everything above it. */
constexpr std::uint64_t unmapped = data_end;
constexpr std::uint64_t fault_base = unmapped + 0x1800;
static_assert(code_bytes < 1024 * 1024,
	"a literal load must reach the unmapped page");

[[noreturn]] void fail(const char *text, const char *scenario,
		       unsigned position, const char *what)
{
	std::fprintf(stderr, "%s [%s, position %u]: %s\n", text, scenario,
		     position, what);
	std::abort();
}

struct Outcome final {
	::uml::cpu::EngineEvent event{::uml::cpu::EngineEvent::fatal};
	arm64::CpuState cpu{};
	std::size_t executed{};
	std::uint64_t fault_address{};
};

[[nodiscard]] Outcome execute_block(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation,
	arm64::CpuState cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event =
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	gadgets::restore_native_fp_environment(cpu);
	return {.event = event, .cpu = cpu, .executed = executed,
		.fault_address = executor.fault_address()};
}

[[nodiscard]] constexpr std::uint64_t slot_address(
	std::size_t index, unsigned which) noexcept
{
	return code_base + (index * slots_per_case + which) * slot;
}

[[nodiscard]] std::uint32_t literal_word(
	std::size_t literal, std::uint64_t at)
{
	const auto delta = static_cast<std::int64_t>(fault_base) -
		static_cast<std::int64_t>(at);
	require(delta % 4 == 0 && delta > 0 && delta < (1 << 20));
	return literal_templates[literal] |
		((static_cast<std::uint32_t>(delta >> 2) & 0x7ffffU) << 5);
}

[[nodiscard]] std::uint32_t direct_branch(
	std::uint64_t source, std::uint64_t target)
{
	const auto displacement = static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

[[nodiscard]] std::uint32_t transfer_word(
	std::size_t index, std::uint64_t at)
{
	return index < std::size(cases) ? cases[index].word :
		literal_word(index - std::size(cases), at);
}

void write_code(JitBacking &backing)
{
	for (std::size_t index = 0; index < case_count; ++index) {
		for (std::size_t which = 0; which < positions.size(); ++which) {
			const auto start = slot_address(index, which) - code_base;
			const auto n = positions[which];
			for (unsigned i = 0; i < n; ++i)
				backing.put32(start + i * 4, pad);
			backing.put32(start + n * 4,
				transfer_word(index, start + code_base + n * 4));
			backing.put32(start + n * 4 + 4, after);
			backing.put32(start + n * 4 + 8, svc);
		}
		/* A branch to the blocks at position 0 and 62. */
		backing.put32(slot_address(index, 3) - code_base,
			direct_branch(slot_address(index, 3),
				      slot_address(index, 0)));
		backing.put32(slot_address(index, 4) - code_base,
			direct_branch(slot_address(index, 4),
				      slot_address(index, 2)));
	}
}

struct Setup final {
	std::uint64_t base;   // the base register
	std::uint64_t stack;  // SP
};

[[nodiscard]] arm64::CpuState initial_state(
	std::uint64_t entry, const Setup &setup)
{
	arm64::CpuState cpu{};
	for (unsigned i = 0; i < 31; ++i)
		cpu.registers[i] = 0x5a5a'0000'0000'0000ULL | i;
	cpu.registers[2] = setup.base;
	cpu.registers[3] = 0;
	cpu.registers[20] = 1000;
	cpu.sp = setup.stack;
	cpu.pc = entry;
	return cpu;
}

/* The faulting instruction retired nothing and wrote nothing: every register
 * but the pad counter, and SP, is as it was. */
void require_fault(const Outcome &outcome, const arm64::CpuState &start,
		   std::uint64_t pc, std::size_t executed, unsigned position,
		   const char *text, const char *scenario)
{
	if (outcome.event != ::uml::cpu::EngineEvent::memory_fault)
		fail(text, scenario, position, "no memory fault");
	if (outcome.cpu.pc != pc)
		fail(text, scenario, position, "wrong PC");
	if (outcome.executed != executed)
		fail(text, scenario, position, "wrong retired count");
	if (outcome.cpu.gadget_indirect_miss != nullptr)
		fail(text, scenario, position, "stream pointer left behind");
	for (unsigned i = 0; i < 31; ++i) {
		const auto expected = i == 20 ? start.registers[20] + position :
			start.registers[i];
		if (outcome.cpu.registers[i] != expected)
			fail(text, scenario, position, "register changed");
	}
	if (outcome.cpu.sp != start.sp)
		fail(text, scenario, position, "SP changed");
}

struct Rig final {
	static constexpr unsigned long generation = 910;
	JitBacking backing{code_pages + data_pages};
	JitMapping mapping;
	StaticGuestMemory memory;
	GadgetExecutor executor;

	Rig()
		: mapping{prepared(backing), code_base, backing,
			  code_pages + data_pages}
	{
	}

	/* The code goes in before the guest can see it. */
	[[nodiscard]] static unsigned long prepared(JitBacking &code)
	{
		write_code(code);
		return generation;
	}

	[[nodiscard]] Outcome run(std::uint64_t entry, const Setup &setup)
	{
		return execute_block(executor, memory, generation,
				     initial_state(entry, setup));
	}
};

void run_case(Rig &rig, std::size_t index)
{
	const bool literal = index >= std::size(cases);
	const char *text = literal ?
		literal_text[index - std::size(cases)] : cases[index].text;
	const unsigned size = literal ? 0 : cases[index].size;
	const bool sp = !literal && cases[index].stack_base;

	for (std::size_t which = 0; which < positions.size(); ++which) {
		const auto entry = slot_address(index, which);
		const unsigned n = positions[which];
		const auto pc = entry + n * 4;

		/* An unmapped page. */
		{
			const Setup setup{fault_base, fault_base};
			const auto start = initial_state(entry, setup);
			const auto outcome = rig.run(entry, setup);
			require_fault(outcome, start, pc, n, n, text,
				      "unmapped");
			if (outcome.fault_address < unmapped ||
			    outcome.fault_address >= unmapped + 0x3000)
				fail(text, "unmapped", n, "wrong fault address");
		}
		if (literal)
			continue;
		const auto &item = cases[index];

		/* An access that runs off the end of the last data page. */
		if (item.size > 1) {
			const auto base = data_end - 1 -
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						item.displacement));
			const Setup setup{base, sp ? base : fault_base};
			const auto start = initial_state(entry, setup);
			const auto outcome = rig.run(entry, setup);
			require_fault(outcome, start, pc, n, n, text,
				      "page crossing");
		}

		/* A single-copy-atomic access across a 16-byte block. */
		if (item.single_copy && !sp && item.size > 1) {
			const auto base = data_base + 0x100 + 16 -
				(item.size / 2);
			const Setup setup{base, fault_base};
			const auto start = initial_state(entry, setup);
			const auto outcome = rig.run(entry, setup);
			require_fault(outcome, start, pc, n, n, text,
				      "misaligned");
		}

		/* An SP that is not 16-byte aligned. */
		if (item.stack_checked) {
			const Setup setup{data_base + 0x108, data_base + 0x108};
			const auto start = initial_state(entry, setup);
			const auto outcome = rig.run(entry, setup);
			require_fault(outcome, start, pc, n, n, text,
				      "misaligned SP");
		}
	}

	/* Reached through a link from another block, which warmed it above:
	 * the branch retires one more instruction. */
	for (const unsigned which : {3U, 4U}) {
		const auto target = slot_address(index, which == 3 ? 0 : 2);
		const unsigned n = which == 3 ? positions[0] : positions[2];
		const auto entry = slot_address(index, which);
		const Setup setup{fault_base, fault_base};
		const auto start = initial_state(entry, setup);
		const auto outcome = rig.run(entry, setup);
		require_fault(outcome, start, target + n * 4, n + 1, n, text,
			      "linked");
	}
	(void)size;
}

/* The block's instruction count is bounded by the position's width: a
 * transfer at position 255 is named, and a longer window is cut there. */
void positions_fill_the_operand_byte()
{
	constexpr std::uint64_t base = 0x8000;
	static_assert(gadgets::fault_position_instructions == 256);
	for (const auto &item : cases) {
		std::vector<std::uint32_t> words(255, pad);
		words.push_back(item.word);
		words.push_back(svc);
		const auto built = gadgets::Builder{}.build(base, words);
		require(built.complete);
		require(built.program.instruction_count() == 256U);
		arm64::CpuState cpu{};
		cpu.registers[2] = 0;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		const auto event =
			gadgets::execute(built.program, cpu, tlb, executed);
		if (event != ::uml::cpu::EngineEvent::memory_fault)
			fail(item.text, "position 255", 255, "no memory fault");
		gadgets::resolve_fault_position(built.program, cpu.pc);
		if (cpu.pc != base + 255 * 4)
			fail(item.text, "position 255", 255, "wrong PC");
	}

	std::vector<std::uint32_t> longer(300, pad);
	const auto cut = gadgets::Builder{}.build(base, longer);
	require(cut.complete);
	require(cut.program.instruction_count() == 256U);
}

/* A literal whose target does not fit the operand's 48 bits keeps its PC
 * word and reports the architectural address. */
void a_literal_below_zero_keeps_its_pc_word()
{
	constexpr std::uint64_t low = 0x4000;
	/* ldr x1, #-(low + 4) wraps to the last word of the address space. */
	const auto delta = -static_cast<std::int64_t>(low + 4);
	const std::array words{
		0x5800'0001U |
			((static_cast<std::uint32_t>(delta >> 2) & 0x7ffffU) << 5),
		svc};
	const auto built = gadgets::Builder{}.build(low, words);
	require(built.complete);
	arm64::CpuState cpu{};
	cpu.registers[1] = 0x1234;
	::uml::memory::Tlb tlb;
	std::size_t executed = 0;
	const auto event = gadgets::execute(built.program, cpu, tlb, executed);
	require(event == ::uml::cpu::EngineEvent::memory_fault);
	require(cpu.pc == low);
	require(cpu.registers[1] == 0x1234);
	require(tlb.fault_address == ~std::uint64_t{3});
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	positions_fill_the_operand_byte();
	a_literal_below_zero_keeps_its_pc_word();
	Rig rig;
	for (std::size_t index = 0; index < case_count; ++index)
		run_case(rig, index);
	std::puts("arm64-gadget-memory-fault-position: ok");
	return 0;
}
