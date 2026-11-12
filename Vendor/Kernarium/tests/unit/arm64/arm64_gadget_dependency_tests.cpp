// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_dependency_reference.hpp"
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <source_location>
#include <span>
#include <vector>

namespace g = uml::cpu::arm64::gadgets;
namespace a = uml::cpu::arm64;
namespace reference = arm64_gadget_dependency_reference;
extern "C" void uml_arm64_gadget_interrupt();
void run_gadget_dependency_memory_tests();
void run_gadget_vector_dependency_tests();
void run_gadget_bitfield_shift_tests();

namespace {
using uml::testing::require;

constexpr bool is_immediate(std::uint32_t word)
{
	return (word & 0x1f80'0000U) == 0x1100'0000U;
}

using uml::emulator::tests::address;

/* Runs a program that ends in SVC through the test-local reference. */
void run_reference(a::CpuState &cpu, std::span<const std::uint32_t> code,
	const std::source_location where = std::source_location::current())
{
	for (auto word : code)
		require(reference::execute(cpu, word) == (word == 0xd400'0001U ?
			reference::Step::system_call : reference::Step::continued), where);
}

std::uint64_t random_word(std::uint64_t &seed)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

void scalar_matrix()
{
	constexpr std::array bases{
		0x1100'0000U, 0x3100'0000U, 0x5100'0000U, 0x7100'0000U,
		0x9100'0000U, 0xb100'0000U, 0xd100'0000U, 0xf100'0000U,
		0x0b00'0000U, 0x2b00'0000U, 0x4b00'0000U, 0x6b00'0000U,
		0x8b00'0000U, 0xab00'0000U, 0xcb00'0000U, 0xeb00'0000U,
		0x0a00'0000U, 0x2a00'0000U, 0x4a00'0000U, 0x6a00'0000U,
		0x8a00'0000U, 0xaa00'0000U, 0xca00'0000U, 0xea00'0000U,
	};
	constexpr std::uint64_t pc = 0x4000;
	const auto encode = [](std::uint32_t base, unsigned destination,
		unsigned left, unsigned right, unsigned immediate) {
		return base | destination | (left << 5) |
			(is_immediate(base) ?
			 (immediate << 10) : (right << 16));
	};
	auto tlb = std::make_unique<uml::memory::Tlb>();
	std::uint64_t seed = 0xced2'8875'abe1'4147ULL;
	std::size_t comparisons = 0;
	for (auto producer : bases)
		for (auto consumer : bases)
			for (unsigned mask = 0; mask < 4; ++mask) {
				if (is_immediate(consumer) && mask > 1)
					continue;
				const auto destination = static_cast<unsigned>(random_word(seed) % 31);
				const auto first = (destination + 1) % 31;
				const auto second = (destination + 2) % 31;
				const auto result = (destination + 3) % 31;
				const std::array instructions{
					encode(producer, destination, first, second, 4095),
					encode(consumer, result, mask & 1 ? destination : first,
						mask & 2 ? destination : second, 13),
					// A second adjacent consumer tests normalization of an
					// already-forwarded immediate, not only isolated pairs.
					0x9100'0400U | first | (result << 5),
					0xd400'0001U,
				};
				const auto enabled = g::Builder{}.build(pc, instructions);
				const auto disabled = g::Builder{}.build(pc, instructions, {},
					std::pmr::get_default_resource(), {}, {.scalar_forwarding = false});
				require(enabled.complete && disabled.complete);
				require(enabled.program.words().size() == disabled.program.words().size());
				if (mask != 0)
					require(enabled.program.words()[2] == g::scalar_access(disabled.program.words().subspan(2, 2)).contract->entries[mask]);
				for (unsigned trial = 0; trial < 4; ++trial) {
					a::CpuState expected{};
					for (auto &value : expected.registers) value = random_word(seed);
					expected.pc = pc;
					expected.sp = random_word(seed) & ~std::uint64_t{15};
					expected.nzcv = static_cast<std::uint32_t>(random_word(seed)) & 0xf000'0000U;
					a::CpuState on = expected, off = expected;
					run_reference(expected, instructions);
					std::size_t on_count{}, off_count{};
					require(g::execute(enabled.program, on, *tlb, on_count) == uml::cpu::EngineEvent::system_call);
					require(g::execute(disabled.program, off, *tlb, off_count) == uml::cpu::EngineEvent::system_call);
					require(on_count == 4 && off_count == 4);
					require(on.registers == expected.registers && off.registers == expected.registers);
					require(on.nzcv == expected.nzcv && off.nzcv == expected.nzcv);
					require(on.pc == expected.pc && off.pc == expected.pc);
					require(on.sp == expected.sp && off.sp == expected.sp);
					++comparisons;
				}
			}
	std::printf("scalar matrix: %zu reference/on/off comparisons\n", comparisons);
}

void constant_boundary()
{
	constexpr std::uint64_t pc = 0x6000;
	const std::array instructions{0x9100'0400U, 0xd288'6420U,
		0x9100'0801U, 0xd400'0001U};
	const auto built = g::Builder{}.build(pc, instructions);
	a::CpuState cpu{};
	auto tlb = std::make_unique<uml::memory::Tlb>();
	require(g::execute(built.program, cpu, *tlb) == uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 0x4321 && cpu.registers[1] == 0x4323);
}

void undeclared_boundary()
{
	// CRC is intentionally outside the declared forwarding families. Its
	// architectural result must be reloaded, not confused with the old x0.
	const std::array instructions{0x9100'0400U, 0x1ac2'4820U,
		0x9100'0801U, 0xd400'0001U};
	a::CpuState expected{};
	expected.pc = 0x6000;
	expected.registers[0] = 123;
	expected.registers[1] = 0xffff'abcd;
	expected.registers[2] = 0x8765'4321;
	auto actual = expected;
	run_reference(expected, instructions);
	const auto built = g::Builder{}.build(0x6000, instructions);
	auto tlb = std::make_unique<uml::memory::Tlb>();
	require(g::execute(built.program, actual, *tlb) == uml::cpu::EngineEvent::system_call);
	require(actual.registers == expected.registers && actual.pc == expected.pc &&
		actual.nzcv == expected.nzcv);
}

g::ScalarAccess access_for(std::uint32_t instruction)
{
	const std::array code{instruction};
	const auto built = g::Builder{}.build(0x4000, code, {},
		std::pmr::get_default_resource(), {}, {.scalar_forwarding = false});
	for (std::size_t count = 1; count <= 4 && count <= built.program.words().size(); ++count) {
		const auto access = g::scalar_access(built.program.words().first(count));
		if (access.contract) return access;
	}
	return {};
}

void catalog()
{
	std::set<std::uintptr_t> entries;
	std::size_t variants{};
	for (const auto &contract : g::scalar_contracts()) {
		require(contract.operand_words >= 1 && contract.operand_words <= 3);
		require(contract.reserved == 0 && entries.insert(contract.entries[0]).second);
		[[maybe_unused]] unsigned available{};
		for (unsigned i = 0; i < contract.sources.size(); ++i)
			if (contract.sources[i] != g::scalar_no_register) available |= 1U << i;
		for (unsigned mask = 1; mask < 8; ++mask) {
			require(bool(contract.entries[mask]) == ((available & mask) == mask));
			if (contract.entries[mask]) {
				require(entries.insert(contract.entries[mask]).second);
				++variants;
			}
		}
	}
	require(g::scalar_contracts().size() > 1000);
	std::printf("scalar catalog: %zu ordinary entries, %zu source-mask variants\n",
		g::scalar_contracts().size(), variants);
}

void broad_matrix()
{
	auto tlb = std::make_unique<uml::memory::Tlb>();
	std::uint64_t seed = 0x6540'cdfe'8302'ab93ULL;
	std::size_t comparisons{};
	std::set<unsigned> families;
	for (unsigned wide = 0; wide < 2; ++wide)
	for (unsigned pattern = 0; pattern < 16; ++pattern) {
		const unsigned n = pattern & 1 ? 0 : 1, m = pattern & 2 ? 0 : 2;
		const unsigned a = pattern & 4 ? 0 : 3, d = pattern & 8 ? 0 : 4;
		const auto sf = wide << 31;
		std::vector<std::uint32_t> words;
		for (unsigned op = 0; op < 4; ++op) {
			words.push_back(sf | 0x1100'0000U | (op << 29) | (4095U << 10) | (n << 5) | d);
			for (unsigned shift = 0; shift < 3; ++shift) {
				words.push_back(sf | 0x0b00'0000U | (op << 29) | (shift << 22) |
					(m << 16) | (17U << 10) | (n << 5) | d);
			}
			for (unsigned extension = 0; extension < 8; ++extension)
				words.push_back(sf | 0x0b20'0000U | (op << 29) | (m << 16) |
					(extension << 13) | (3U << 10) | (n << 5) | d);
			words.push_back(sf | 0x1a00'0000U | (op << 29) | (m << 16) | (n << 5) | d);
			for (unsigned shift = 0; shift < 4; ++shift)
				for (unsigned invert = 0; invert < 2; ++invert)
					words.push_back(sf | 0x0a00'0000U | (op << 29) | (shift << 22) |
						(invert << 21) | (m << 16) | (7U << 10) | (n << 5) | d);
			words.push_back(sf | 0x1200'0000U | (op << 29) | (wide << 22) |
				(3U << 16) | (12U << 10) | (n << 5) | d);
			words.push_back(sf | 0x1ac0'2000U | (op << 10) | (m << 16) | (n << 5) | d);
			// A 64-bit in-place equal-input min/max is decoded as NOP,
			// which deliberately has no scalar access contract.
			const auto minmax_d = wide && n == m && d == n ? 4U : d;
			words.push_back(sf | 0x1ac0'6000U | (op << 10) | (m << 16) | (n << 5) | minmax_d);
			words.push_back(sf | 0x11c0'0000U | (op << 18) | (0x81U << 10) | (n << 5) | d);
			for (unsigned condition = 0; condition < 16; ++condition)
				words.push_back(sf | 0x3a40'0000U | ((op & 1) << 30) |
					((op >> 1) << 11) | (m << 16) | (condition << 12) | (n << 5) | 9U);
			for (unsigned cond = 0; cond < 16; ++cond)
				words.push_back(sf | 0x1a80'0000U | ((op >> 1) << 30) | ((op & 1) << 10) |
					(m << 16) | (cond << 12) | (n << 5) | d);
		}
		for (unsigned op = 0; op < 3; ++op)
			for (unsigned rotate : {0U, 7U, 23U})
				words.push_back(sf | 0x1300'0000U | (op << 29) | (wide << 22) |
					(rotate << 16) | (15U << 10) | (n << 5) | d);
		for (unsigned half = 0; half < (wide ? 4U : 2U); ++half)
			words.push_back(sf | 0x7280'0000U | (half << 21) | (0xabcdU << 5) | d);
		for (unsigned op : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
			if (!wide && op == 3) continue;
			words.push_back(sf | 0x5ac0'0000U | (op << 10) | (n << 5) | d);
		}
		words.push_back(sf | 0x1380'0000U | (wide << 22) | (m << 16) |
			(13U << 10) | (n << 5) | d);
		for (unsigned subtract = 0; subtract < 2; ++subtract) {
			words.push_back(sf | 0x1b00'0000U | (m << 16) | (subtract << 15) |
				(a << 10) | (n << 5) | d);
			if (wide) {
				for (unsigned long_op : {1U, 5U})
					words.push_back(sf | 0x1b00'0000U | (long_op << 21) | (m << 16) |
						(subtract << 15) | (a << 10) | (n << 5) | d);
			}
			words.push_back(sf | 0x1ac0'0800U | (subtract << 10) | (m << 16) | (n << 5) | d);
		}
		// Compare aliases (discarded destination), SP, ZR/copy and equal-source cases.
		words.push_back(sf | 0x7100'001fU | (13U << 10) | (n << 5));
		words.push_back(0x9100'001fU | (16U << 10) | (n << 5));
		words.push_back(0x9100'0004U | (16U << 10) | (31U << 5));
		words.push_back(sf | 0x2a00'0000U | (m << 16) | (31U << 5) | d);
		words.push_back(sf | 0x1a00'0000U | (n << 16) | (n << 5) | d);
		for (auto word : words) {
			const auto access = access_for(word);
			if (!access.contract) {
				std::fprintf(stderr, "missing scalar contract for %08x\n", word);
				std::abort();
			}
			families.insert(access.contract->family);
			// Change each source through a real producer, then consume this
			// instruction's own result as well. The test-local reference is
			// the oracle.
			std::set<std::uint16_t> sources{0};
			for (auto source : access.sources)
				if (source < 31 * 8 || source == offsetof(a::CpuState, sp)) sources.insert(source);
			for (auto source : sources) {
				const auto rn = source == offsetof(a::CpuState, sp) ? 31U : source / 8U;
				const auto result_rn = access.destination == offsetof(a::CpuState, sp) ? 31U :
					access.destination == g::scalar_no_register ? 6U : access.destination / 8U;
				const std::array code{
					0x9100'4000U | (rn << 5) | rn,
					word,
					0x9100'0405U | (result_rn << 5),
					0xd400'0001U,
				};
				const auto on = g::Builder{}.build(0x4000, code);
				const auto off = g::Builder{}.build(0x4000, code, {},
					std::pmr::get_default_resource(), {}, {.scalar_forwarding = false});
				for (unsigned trial = 0; trial < 4; ++trial) {
					a::CpuState expected{};
					for (auto &value : expected.registers) value = random_word(seed);
					expected.sp = random_word(seed) & ~std::uint64_t{15};
					expected.nzcv = ((trial + pattern) & 15U) << 28;
					expected.pc = 0x4000;
					auto actual_on = expected, actual_off = expected;
					run_reference(expected, code);
					for (const auto &[program, actual] : {
						std::pair{&on.program, &actual_on}, std::pair{&off.program, &actual_off}}) {
						std::size_t count{};
						require(g::execute(*program, *actual, *tlb, count) == uml::cpu::EngineEvent::system_call);
						if (actual->registers != expected.registers || actual->sp != expected.sp ||
							actual->nzcv != expected.nzcv || actual->pc != expected.pc || count != 4) {
							std::fprintf(stderr, "broad mismatch %08x source=%u trial=%u forwarding=%d\n",
								word, source, trial, actual == &actual_on);
							std::abort();
						}
					}
					++comparisons;
				}
			}
		}
	}
	require(families.size() >= 16);
	std::printf("broad scalar matrix: %zu reference/on/off comparisons across %zu families\n",
		comparisons, families.size());
}

void linked_counts()
{
	constexpr std::uint64_t pc = 0x1000;
	constexpr std::uint64_t iterations = 17;
	std::array<std::uintptr_t, 2> terminal{address(uml_arm64_gadget_interrupt), pc + 20};
	g::BlockLink loop{.guest_pc = pc};
	g::BlockLink end{.stream = terminal.data(), .instruction_count = 0, .guest_pc = pc + 20};
	const std::array instructions{0x9100'0400U, 0x9100'0801U, 0x8b00'0022U,
		0xf100'0463U, 0x54ff'ff81U};
	auto built = g::Builder{}.build(pc, instructions,
		[&](std::uint64_t target) -> const g::BlockLink * {
			if (target == pc) return &loop;
			require(target == pc + 20);
			return &end;
		});
	require(built.complete && built.program.instruction_count() == 5);
	loop.stream = built.program.words().data();
	loop.instruction_count = 5;
	a::CpuState cpu{};
	cpu.registers[3] = iterations;
	auto tlb = std::make_unique<uml::memory::Tlb>();
	std::size_t executed{};
	const auto event = g::execute(built.program, cpu, *tlb, executed);
	require(event == uml::cpu::EngineEvent::interrupt);
	require(executed == 5 * iterations && cpu.registers[3] == 0);
	require(cpu.registers[0] == iterations && cpu.registers[1] == iterations + 2);
	require(cpu.registers[2] == 2 * iterations + 2);
}

void taken_prefix()
{
	constexpr std::uint64_t pc = 0x2000, target = pc + 0x100;
	std::array<std::uintptr_t, 2> terminal{address(uml_arm64_gadget_interrupt), target};
	g::BlockLink end{.stream = terminal.data(), .instruction_count = 0, .guest_pc = target};
	const std::array instructions{0x9100'0400U,
		0xb400'0004U | (63U << 5), 0x9100'0801U};
	auto built = g::Builder{}.build(pc, instructions,
		[&](std::uint64_t) -> const g::BlockLink * { return &end; });
	a::CpuState cpu{};
	auto tlb = std::make_unique<uml::memory::Tlb>();
	std::size_t executed{};
	require(g::execute(built.program, cpu, *tlb, executed) == uml::cpu::EngineEvent::interrupt);
	require(executed == 2 && cpu.registers[0] == 1 && cpu.registers[1] == 0);
}

} // namespace

int main()
{
	catalog();
	broad_matrix();
	scalar_matrix();
	constant_boundary();
	undeclared_boundary();
	linked_counts();
	taken_prefix();
	run_gadget_dependency_memory_tests();
	run_gadget_bitfield_shift_tests();
	run_gadget_vector_dependency_tests();
	std::puts("gadget dependency tests passed");
}
