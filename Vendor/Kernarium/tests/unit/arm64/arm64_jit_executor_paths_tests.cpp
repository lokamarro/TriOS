// SPDX-License-Identifier: GPL-2.0-or-later
/* The paths the JIT executor's A64 and AArch32 entries run only when a block
 * is missing, when another vCPU is publishing, or when a step must be small:
 *
 * - a second executor reuses the first one's A32 and T32 translations, and
 *   still finds code rewritten under them;
 * - while another vCPU holds a reservation, a block that stores runs as its
 *   tracked variant, built from the code now in memory, and falls back to one
 *   instruction when that code no longer matches the block;
 * - a single step, and an entry in the middle of an IT block, run one
 *   instruction as its own translation, tracked or not;
 * - a fetch that cannot supply the first instruction reports the fault at the
 *   first unit it could not read, whichever entry or fetch ran;
 * - a block ends where its mapping ends, and an AArch32 block where the 32-bit
 *   address space ends, whatever lies beyond. */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
namespace key = uml::cpu::arm64::aarch32;
using uml::cpu::EngineEvent;
using uml::cpu::arm64::JitExecutor;
using uml::memory::Access;
using uml::memory::StaticGuestMemory;

constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t code_base = 0x00f0'0000U;
constexpr std::uint32_t data_offset = 2U * page;
constexpr std::uint32_t stored_value = 0x1234'5678U;

constexpr std::uint32_t a32_add_r0 = 0xe280'0001U;  // add r0, r0, #1
constexpr std::uint32_t a32_str = 0xe582'1000U;     // str r1, [r2]
constexpr std::uint32_t a32_str_4 = 0xe582'1004U;   // str r1, [r2, #4]
constexpr std::uint32_t a32_svc = 0xef00'0000U;     // svc #0
constexpr std::uint16_t t32_adds_r0 = 0x3001;       // adds r0, #1
constexpr std::uint16_t t32_str = 0x6011;           // str r1, [r2]
constexpr std::uint16_t t32_svc = 0xdf00;           // svc #0
constexpr std::uint16_t t32_str_w[] = {0xf8c2, 0x1000};   // str.w r1, [r2]
constexpr std::uint16_t t32_str_w_4[] = {0xf8c2, 0x1004}; // str.w r1, [r2, #4]
constexpr std::uint32_t a64_add_x0 = 0x9100'0400U;  // add x0, x0, #1
constexpr std::uint32_t a64_str = 0xf900'0041U;     // str x1, [x2]
constexpr std::uint32_t a64_svc = 0xd400'0001U;     // svc #0
constexpr std::uint32_t a64_ldxr = 0xc85f'7c23U;    // ldxr x3, [x1]
constexpr std::uint32_t a64_str_x1 = 0xf900'0022U;  // str x2, [x1]
constexpr std::uint32_t a64_stxr = 0xc805'7c24U;    // stxr w5, x4, [x1]
constexpr std::uint32_t a32_ldrex = 0xe190'2f9fU;   // ldrex r2, [r0]
constexpr std::uint32_t a32_str_r0 = 0xe580'1000U;  // str r1, [r0]
constexpr std::uint32_t a32_strex = 0xe180'3f92U;   // strex r3, r2, [r0]
constexpr std::uint16_t t32_ldrex[] = {0xe850, 0x2f00}; // ldrex r2, [r0]
constexpr std::uint16_t t32_str_r0 = 0x6001;            // str r1, [r0]
constexpr std::uint16_t t32_strex[] = {0xe840, 0x2300}; // strex r3, r2, [r0]

enum class Isa { a64, a32, t32 };

/* A code region of `pages` pages at `base` (the page after the code is data
 * when `pages` is three), writable and executable, in a generation of its
 * own. */
class Region final {
public:
	Region(unsigned long generation, std::uint32_t base, std::size_t pages = 3)
		: generation_{generation}, base_{base}, backing_{pages},
		  mapping_{generation, base, backing_, pages}
	{
	}

	void put32(std::size_t offset, std::uint32_t word)
	{
		backing_.put32(offset, word);
	}
	void put16(std::size_t offset, std::uint16_t half)
	{
		backing_.put(offset, std::as_bytes(std::span{&half, 1}));
	}
	[[nodiscard]] std::uint32_t get32(std::size_t offset) const
	{
		std::uint32_t word;
		backing_.get(offset, std::as_writable_bytes(std::span{&word, 1}));
		return word;
	}
	/* Code written by a guest store, which publishes a change. */
	void changed(std::size_t offset, std::size_t length) const
	{
		require(darwin_uml_static_vm_invalidate_code_generation(
			generation_, base_ + offset, length) == 0);
	}

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}
	[[nodiscard]] std::uint32_t base() const noexcept { return base_; }

private:
	unsigned long generation_;
	std::uint32_t base_;
	JitBacking backing_;
	JitMapping mapping_;
};

/* The task about to run `isa` code at `address`: r1 holds the value its store
 * writes and r2 (x2) the data word. */
[[nodiscard]] arm64::CpuState task(Isa isa, std::uint32_t address,
	std::uint32_t base)
{
	arm64::CpuState cpu{};
	cpu.registers[1] = stored_value;
	cpu.registers[2] = base + data_offset;
	if (isa == Isa::a64) {
		cpu.pc = address;
		return cpu;
	}
	cpu.a32 = 1;
	cpu.pc = isa == Isa::t32 ? key::t32_key(address) : key::a32_key(address);
	return cpu;
}

struct Outcome final {
	EngineEvent event{};
	std::size_t executed{};
};

/* One entry into `executor`, through the A64 or the AArch32 block entry the
 * task's ISA selects. */
[[nodiscard]] Outcome enter(JitExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, Isa isa, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	Outcome outcome;
	outcome.event = isa == Isa::a64 ?
		executor.run_block(cpu, memory, generation, outcome.executed) :
		executor.run_a32_block(cpu, memory, generation, outcome.executed);
	executor.end_execution();
	return outcome;
}

/* Entries until the program's SVC; the sum of the retired counts. */
[[nodiscard]] std::size_t run_to_svc(JitExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, Isa isa,
	arm64::CpuState &cpu)
{
	std::size_t retired = 0;
	for (unsigned entry = 0; entry < 16; ++entry) {
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		retired += outcome.executed;
		if (outcome.event == EngineEvent::system_call)
			return retired;
		require(outcome.event == EngineEvent::continued);
	}
	require(false);
	return retired;
}

/* The ISA's program: add; store; add; svc (four instructions). The T32 form
 * stores with the 32-bit encoding so that it holds five halfwords. */
void write_tracking_program(Region &region, Isa isa, std::size_t offset = 0)
{
	switch (isa) {
	case Isa::a64:
		region.put32(offset, a64_add_x0);
		region.put32(offset + 4, a64_str);
		region.put32(offset + 8, a64_add_x0);
		region.put32(offset + 12, a64_svc);
		break;
	case Isa::a32:
		region.put32(offset, a32_add_r0);
		region.put32(offset + 4, a32_str);
		region.put32(offset + 8, a32_add_r0);
		region.put32(offset + 12, a32_svc);
		break;
	case Isa::t32:
		region.put16(offset, t32_adds_r0);
		region.put16(offset + 2, t32_str_w[0]);
		region.put16(offset + 4, t32_str_w[1]);
		region.put16(offset + 6, t32_adds_r0);
		region.put16(offset + 8, t32_svc);
		break;
	}
	region.changed(offset, 16);
}

[[nodiscard]] const char *name(Isa isa) noexcept
{
	return isa == Isa::a64 ? "A64" : isa == Isa::a32 ? "A32" : "T32";
}

/* The value the program's store left in the data word, and its reset. */
[[nodiscard]] std::uint32_t data_word(const Region &region)
{
	return region.get32(data_offset);
}

/* A reservation another vCPU holds makes every direct store publish its
 * ranges: the executor runs the tracked variant of a block that stores. */
class ForeignReservation final {
public:
	ForeignReservation() { require(arm64::begin_jit_reservation(cpu_)); }
	~ForeignReservation() { arm64::end_jit_reservation(cpu_); }
	ForeignReservation(const ForeignReservation &) = delete;
	ForeignReservation &operator=(const ForeignReservation &) = delete;

private:
	arm64::CpuState cpu_{};
};

/* A block that stores runs untracked while nobody reserves, and as its
 * tracked variant while another vCPU does; both leave the same state. */
void tracked_variant_runs_the_block(Isa isa, unsigned long generation)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	StaticGuestMemory memory;
	const auto address = code_base;
	{
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
		require(cpu.registers[0] == 2);
		require(data_word(region) == stored_value);
	}
	region.put32(data_offset, 0);
	{
		/* One entry runs the whole block, so the tracked variant was
		 * built (the first round) and found again (the second). */
		ForeignReservation foreign;
		for (unsigned round = 0; round < 2; ++round) {
			auto cpu = task(isa, address, code_base);
			const auto outcome =
				enter(executor, memory, generation, isa, cpu);
			require(outcome.event == EngineEvent::system_call);
			require(outcome.executed == 4);
			require(cpu.registers[0] == 2);
			require(data_word(region) == stored_value);
			region.put32(data_offset, 0);
		}
	}
	std::printf("tracked variant runs the %s block\n", name(isa));
}

/* How code rewritten behind the cache's back differs from the block: it ends
 * sooner (fewer instructions and units), it has fewer units for the same
 * instructions, more instructions in the same units, or it no longer stores
 * (the same instructions and units, other writes). */
enum class Rewrite { shorter, fewer_units, more_instructions, no_store };

[[nodiscard]] const char *name(Rewrite rewrite) noexcept
{
	switch (rewrite) {
	case Rewrite::shorter:
		return "shorter";
	case Rewrite::fewer_units:
		return "fewer units";
	case Rewrite::more_instructions:
		return "more instructions";
	case Rewrite::no_store:
		return "storeless";
	}
	return "";
}

/* The tracked variant is built again from the code in memory. Code rewritten
 * without a published change leaves the cached block valid for an untracked
 * run, but a tracked run compiles something else and so makes progress one
 * instruction at a time instead. */
void tracked_variant_that_no_longer_matches_runs_one_instruction(Isa isa,
	unsigned long generation, Rewrite rewrite)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	StaticGuestMemory memory;
	const auto address = code_base;
	{
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
	}
	region.put32(data_offset, 0);
	/* The programs are add; store; add; svc, the T32 one with its 32-bit
	 * store in halfwords 1 and 2 and its SVC in halfword 4. */
	constexpr std::uint16_t t32_add_w[] = {0xf100, 0x0001}; // add.w r0, r0, #1
	switch (rewrite) {
	case Rewrite::shorter:
		if (isa == Isa::t32)
			region.put16(6, t32_svc);
		else
			region.put32(8, isa == Isa::a64 ? a64_svc : a32_svc);
		break;
	case Rewrite::fewer_units:
		region.put16(2, t32_str);
		region.put16(4, t32_adds_r0);
		region.put16(6, t32_svc);
		break;
	case Rewrite::more_instructions:
		/* adds; str; adds; adds; svc, in the five halfwords. */
		region.put16(2, t32_str);
		region.put16(4, t32_adds_r0);
		break;
	case Rewrite::no_store:
		if (isa == Isa::t32) {
			region.put16(2, t32_add_w[0]);
			region.put16(4, t32_add_w[1]);
		} else {
			region.put32(4, isa == Isa::a64 ? a64_add_x0 : a32_add_r0);
		}
		break;
	}
	{
		/* Nobody reserves: the cached block runs as it was. */
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
		require(cpu.registers[0] == 2);
		require(data_word(region) == stored_value);
		region.put32(data_offset, 0);
	}
	{
		ForeignReservation foreign;
		auto cpu = task(isa, address, code_base);
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		/* The block's first instruction, as its own translation. */
		require(outcome.event == EngineEvent::continued);
		require(outcome.executed == 1);
		require(cpu.registers[0] == 1);
		require(data_word(region) == 0);
		/* And the rest runs on from there. */
		const auto rest = run_to_svc(executor, memory, generation, isa, cpu);
		require(rest >= 2);
		require(data_word(region) ==
			(rewrite == Rewrite::no_store ? 0 : stored_value));
	}
	std::printf("tracked variant of a %s %s block runs one instruction\n",
		name(rewrite), name(isa));
}

/* A step that stores runs as a one-instruction translation, and the tracked
 * form of it while another vCPU reserves. */
void single_step_stores(Isa isa, unsigned long generation)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	executor.set_single_step(true);
	StaticGuestMemory memory;
	for (const bool tracked : {false, true}) {
		region.put32(data_offset, 0);
		auto cpu = task(isa, code_base, code_base);
		std::size_t entries = 0;
		std::size_t retired = 0;
		const auto step = [&] {
			const auto outcome = enter(executor, memory, generation, isa, cpu);
			require(outcome.executed == 1);
			retired += outcome.executed;
			++entries;
			return outcome.event;
		};
		const auto run = [&] {
			EngineEvent event = EngineEvent::continued;
			while (event == EngineEvent::continued && entries < 8)
				event = step();
			require(event == EngineEvent::system_call);
			require(retired == 4 && entries == 4);
			require(cpu.registers[0] == 2);
			require(data_word(region) == stored_value);
		};
		if (tracked) {
			ForeignReservation foreign;
			run();
		} else {
			run();
		}
	}
	std::printf("single-stepped %s stores\n", name(isa));
}

/* One entry in single-step mode: exactly one instruction retires. */
[[nodiscard]] EngineEvent step_once(JitExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, Isa isa,
	arm64::CpuState &cpu)
{
	std::size_t executed = 0;
	const auto event = isa == Isa::a64 ?
		executor.run_block(cpu, memory, generation, executed) :
		executor.run_a32_block(cpu, memory, generation, executed);
	require(executed == 1);
	return event;
}

/* A plain store from a stepped vCPU, while another holds a reservation on the
 * word, ends that reservation: the store-exclusive after it fails, and
 * succeeds when nothing stored in between. Both vCPUs step one instruction
 * at a time, so the first's load-exclusive runs through the semantic helper
 * and its reservation outlives the entry. */
void a_stepped_store_ends_another_vcpus_reservation(Isa isa, bool intervening,
	unsigned long generation)
{
	Region region{generation, code_base};
	const std::uint32_t data = code_base + data_offset;
	const auto program = [&](std::size_t offset, auto... words) {
		std::size_t at = offset;
		if (isa == Isa::t32)
			((region.put16(at, static_cast<std::uint16_t>(words)), at += 2), ...);
		else
			((region.put32(at, static_cast<std::uint32_t>(words)), at += 4), ...);
	};
	if (isa == Isa::a64) {
		program(0, a64_ldxr);
		program(0x40, a64_str_x1);
		program(0x80, a64_stxr);
	} else if (isa == Isa::a32) {
		program(0, a32_ldrex);
		program(0x40, a32_str_r0);
		program(0x80, a32_strex);
	} else {
		program(0, t32_ldrex[0], t32_ldrex[1]);
		program(0x40, t32_str_r0);
		program(0x80, t32_strex[0], t32_strex[1]);
	}
	region.changed(0, 0x100);
	/* The intervening store writes the value the word already holds, so
	 * only the reservation, not a changed value, can fail the store-
	 * exclusive. */
	region.put32(data_offset, stored_value);

	const auto cpu_at = [&](std::size_t offset) {
		auto cpu = task(isa, code_base + static_cast<std::uint32_t>(offset),
			code_base);
		if (isa == Isa::a64) {
			cpu.registers[1] = data;
			cpu.registers[2] = stored_value;
			cpu.registers[4] = 0x2222'2222U;
		} else {
			cpu.registers[0] = data;
			cpu.registers[1] = stored_value;
			cpu.registers[2] = 0x2222'2222U;
		}
		return cpu;
	};
	JitExecutor first;
	JitExecutor second;
	first.set_single_step(true);
	second.set_single_step(true);
	StaticGuestMemory first_memory;
	StaticGuestMemory second_memory;
	/* A vCPU's reservation is released when its executor stops executing, so
	 * both stay in execution throughout. */
	require(first.begin_execution(generation, code_base));
	require(second.begin_execution(generation, code_base));
	if (intervening) {
		/* The second vCPU's TLB holds the data page before the reservation
		 * exists, so its store is the executor's own, not the helper that
		 * a TLB miss runs (which ends reservations by itself). */
		auto second_cpu = cpu_at(0x40);
		require(step_once(second, second_memory, generation, isa,
			second_cpu) == EngineEvent::continued);
	}
	auto first_cpu = cpu_at(0);
	require(step_once(first, first_memory, generation, isa, first_cpu) ==
		EngineEvent::continued);
	require(first_cpu.exclusive_size != 0);
	if (intervening) {
		auto second_cpu = cpu_at(0x40);
		require(step_once(second, second_memory, generation, isa,
			second_cpu) == EngineEvent::continued);
	}
	const auto next = cpu_at(0x80);
	first_cpu.pc = next.pc;
	if (isa != Isa::a64) {
		/* The store-exclusive stores r2, which the load-exclusive loaded. */
		first_cpu.registers[2] = 0x2222'2222U;
	}
	require(step_once(first, first_memory, generation, isa, first_cpu) ==
		EngineEvent::continued);
	const auto status = first_cpu.registers[isa == Isa::a64 ? 5 : 3];
	require(status == (intervening ? 1U : 0U));
	require(region.get32(data_offset) ==
		(intervening ? stored_value : 0x2222'2222U));
	require(region.get32(data_offset + 4) == 0);
	require(first_cpu.exclusive_size == 0);
	second.end_execution();
	first.end_execution();
	std::printf("%s stepped store %s a reservation\n", name(isa),
		intervening ? "ends" : "leaves");
}

/* An entry in the middle of an IT block runs one instruction as its own
 * translation with the ITSTATE it holds, and leaves the ITSTATE that follows.
 * ITSTATE 0xe8 is "IT AL": the store is the block's only instruction. */
void it_block_entry_stores(bool tracked, unsigned long generation)
{
	Region region{generation, code_base};
	region.put16(0, t32_str);
	region.put16(2, t32_svc);
	region.changed(0, 4);
	JitExecutor executor;
	StaticGuestMemory memory;
	auto cpu = task(Isa::t32, code_base, code_base);
	cpu.a32_itstate = 0xe8U;
	const auto run = [&] {
		const auto outcome =
			enter(executor, memory, generation, Isa::t32, cpu);
		require(outcome.event == EngineEvent::continued);
		require(outcome.executed == 1);
		require(cpu.a32_itstate == 0);
		require(data_word(region) == stored_value);
	};
	if (tracked) {
		ForeignReservation foreign;
		run();
	} else {
		run();
	}
	std::printf("IT-block entry stores%s\n", tracked ? " (tracked)" : "");
}

/* A second executor installs what the first published, for A32 and for T32
 * (whose instructions are one or two halfwords, and whose identity is a
 * count of halfwords), and a rewrite of the code under it is found: of the
 * store, or of the last unit of the identity, which a count that fell short
 * would not compare. */
void a_peer_reuses_aarch32_translations(Isa isa, bool even_units,
	bool rewrite_last, unsigned long generation, std::uint32_t base)
{
	Region region{generation, base};
	/* add; store; svc. The T32 form is four halfwords (the 32-bit store
	 * between two 16-bit instructions), or, without the add, three: an odd
	 * identity that does not fill its last 32-bit element. */
	if (isa == Isa::a32) {
		region.put32(0, a32_add_r0);
		region.put32(4, a32_str);
		region.put32(8, a32_svc);
	} else if (even_units) {
		region.put16(0, t32_adds_r0);
		region.put16(2, t32_str_w[0]);
		region.put16(4, t32_str_w[1]);
		region.put16(6, t32_svc);
	} else {
		region.put16(0, t32_str_w[0]);
		region.put16(2, t32_str_w[1]);
		region.put16(4, t32_svc);
	}
	region.changed(0, 16);
	auto &shared = arm64::SharedTranslationCache::instance();
	StaticGuestMemory memory;
	const auto before = shared.statistics();
	/* Sharing needs two live executors; the second runs only after the
	 * first is gone. */
	JitExecutor second;
	const std::size_t instructions = isa == Isa::a32 || even_units ? 3 : 2;
	{
		JitExecutor first;
		auto cpu = task(isa, base, base);
		require(run_to_svc(first, memory, generation, isa, cpu) ==
			instructions);
		require(data_word(region) == stored_value);
	}
	const auto published = shared.statistics();
	require(published.insertions > before.insertions);
	region.put32(data_offset, 0);
	{
		auto cpu = task(isa, base, base);
		require(run_to_svc(second, memory, generation, isa, cpu) ==
			instructions);
		require(data_word(region) == stored_value);
		if (isa == Isa::a32 || even_units)
			require(cpu.registers[0] == 1);
	}
	const auto reused = shared.statistics();
	require(reused.reuses > published.reuses);

	region.put32(data_offset, 0);
	if (rewrite_last) {
		/* The SVC becomes an undefined instruction. */
		const std::size_t last = isa == Isa::a32 ? 8 : even_units ? 6 : 4;
		if (isa == Isa::a32) {
			region.put32(last, 0xe7f0'00f0U); // udf #0
			region.changed(last, 4);
		} else {
			region.put16(last, 0xde00); // udf #0
			region.changed(last, 2);
		}
		auto cpu = task(isa, base, base);
		const auto outcome = enter(second, memory, generation, isa, cpu);
		require(outcome.event == EngineEvent::illegal_instruction);
	} else {
		/* The store's displacement changes, and publishes the change: the
		 * second executor, which holds the reused block, finds it. */
		if (isa == Isa::a32) {
			region.put32(4, a32_str_4);
			region.changed(4, 4);
		} else {
			const std::size_t at = even_units ? 2 : 0;
			region.put16(at, t32_str_w_4[0]);
			region.put16(at + 2, t32_str_w_4[1]);
			region.changed(at, 4);
		}
		auto cpu = task(isa, base, base);
		require(run_to_svc(second, memory, generation, isa, cpu) ==
			instructions);
		require(region.get32(data_offset) == 0);
		require(region.get32(data_offset + 4) == stored_value);
	}
	std::printf("peer reuses %s%s translations, rewrites the %s\n", name(isa),
		isa == Isa::t32 ? (even_units ? " (even)" : " (odd)") : "",
		rewrite_last ? "last unit" : "store");
}

/* The first instruction cannot be fetched: the fault is at the first unit
 * the fetch could not read, an execute access, and nothing retires. */
void a_fetch_fault_is_reported(Isa isa, bool single_step,
	unsigned long generation)
{
	/* One page of code: the instruction under test is the last word (A32) or
	 * 32-bit pair of halfwords (T32), starting a halfword before the end. */
	Region region{generation, code_base, 1};
	const std::size_t offset = page - (isa == Isa::t32 ? 2U : 0U);
	if (isa == Isa::t32)
		region.put16(offset, t32_str_w[0]);
	region.changed(0, page);
	JitExecutor executor;
	executor.set_single_step(single_step);
	StaticGuestMemory memory;
	for (const bool unmapped_entry : {true, false}) {
		const std::uint32_t address = unmapped_entry ?
			code_base + 8U * static_cast<std::uint32_t>(page) :
			code_base + static_cast<std::uint32_t>(offset);
		if (!unmapped_entry && isa == Isa::a32)
			continue;
		auto cpu = task(isa, address, code_base);
		const auto before = cpu.pc;
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		require(outcome.event == EngineEvent::memory_fault);
		require(outcome.executed == 0);
		require(cpu.pc == before);
		require(executor.fault_access() == Access::execute);
		/* The unmapped page, or the page after the straddling halfword. */
		require(executor.fault_address() ==
			(unmapped_entry ? address : code_base + page));
	}
	std::printf("%s fetch fault%s\n", name(isa),
		single_step ? " (single step)" : "");
}

/* A block ends where the mapped code does: the SVC is the last word of the
 * only page, and the fetch past it is a fault the block never reports. */
void a_block_ends_at_the_end_of_the_mapping(Isa isa, unsigned long generation)
{
	Region region{generation, code_base, 1};
	std::size_t offset = page - (isa == Isa::t32 ? 4U : 8U);
	if (isa == Isa::t32) {
		region.put16(offset, t32_adds_r0);
		region.put16(offset + 2, t32_svc);
	} else {
		region.put32(offset, isa == Isa::a32 ? a32_add_r0 : a64_add_x0);
		region.put32(offset + 4, isa == Isa::a32 ? a32_svc : a64_svc);
	}
	region.changed(0, page);
	JitExecutor executor;
	StaticGuestMemory memory;
	auto cpu = task(isa, code_base + static_cast<std::uint32_t>(offset),
		code_base);
	require(run_to_svc(executor, memory, generation, isa, cpu) == 2);
	require(cpu.registers[0] == 1);
	std::printf("%s block ends at the end of the mapping\n", name(isa));
}

/* A block never runs past the end of the 32-bit address space, even where
 * the guest memory beyond it holds more code: the last page of the space
 * and the page after it are mapped, and the program has no branch. */
void a_block_stops_at_the_end_of_the_address_space(Isa isa,
	unsigned long generation)
{
	constexpr std::uint32_t last_page = 0xffff'f000U;
	Region region{generation, last_page, 2};
	const std::size_t unit = isa == Isa::t32 ? 2U : 4U;
	const std::size_t adds = 8U / unit;
	for (std::size_t index = 0; index < adds; ++index) {
		if (isa == Isa::t32)
			region.put16(page - 8U + index * unit, t32_adds_r0);
		else
			region.put32(page - 8U + index * unit, a32_add_r0);
	}
	/* What lies beyond: another add and the system call. */
	if (isa == Isa::t32) {
		region.put16(page, t32_adds_r0);
		region.put16(page + 2U, t32_svc);
	} else {
		region.put32(page, a32_add_r0);
		region.put32(page + 4U, a32_svc);
	}
	region.changed(0, 2U * page);
	JitExecutor executor;
	StaticGuestMemory memory;
	auto cpu = task(isa, last_page + static_cast<std::uint32_t>(page - 8U),
		last_page);
	const auto outcome = enter(executor, memory, generation, isa, cpu);
	require(outcome.event == EngineEvent::continued,
		"a block ran into the memory past the 32-bit address space");
	require(outcome.executed == adds && cpu.registers[0] == adds);
	std::printf("%s block stops at the end of the address space\n",
		name(isa));
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	constexpr std::size_t gib = std::size_t{1024} * 1024U * 1024U;
	require(arm64::set_jit_cache_budget(1U * gib, 3));
	unsigned long generation = 0x30a3'2000UL;
	for (const auto isa : {Isa::a64, Isa::a32, Isa::t32}) {
		tracked_variant_runs_the_block(isa, generation++);
		for (const auto rewrite : {Rewrite::shorter, Rewrite::no_store})
			tracked_variant_that_no_longer_matches_runs_one_instruction(
				isa, generation++, rewrite);
		single_step_stores(isa, generation++);
		a_block_ends_at_the_end_of_the_mapping(isa, generation++);
	}
	for (const auto isa : {Isa::a32, Isa::t32})
		a_block_stops_at_the_end_of_the_address_space(isa, generation++);
	for (const auto rewrite :
	     {Rewrite::fewer_units, Rewrite::more_instructions})
		tracked_variant_that_no_longer_matches_runs_one_instruction(
			Isa::t32, generation++, rewrite);
	for (const auto isa : {Isa::a64, Isa::a32, Isa::t32})
		for (const bool intervening : {false, true})
			a_stepped_store_ends_another_vcpus_reservation(
				isa, intervening, generation++);
	it_block_entry_stores(false, generation++);
	it_block_entry_stores(true, generation++);
	std::uint32_t base = code_base;
	for (const bool rewrite_last : {false, true}) {
		a_peer_reuses_aarch32_translations(Isa::a32, false, rewrite_last,
			generation++, base += 0x10000U);
		a_peer_reuses_aarch32_translations(Isa::t32, true, rewrite_last,
			generation++, base += 0x10000U);
		a_peer_reuses_aarch32_translations(Isa::t32, false, rewrite_last,
			generation++, base += 0x10000U);
	}
	for (const auto isa : {Isa::a32, Isa::t32})
		for (const bool single_step : {false, true})
			a_fetch_fault_is_reported(isa, single_step, generation++);
	return 0;
}
