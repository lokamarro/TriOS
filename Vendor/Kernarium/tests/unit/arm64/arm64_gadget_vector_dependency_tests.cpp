// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_dependency_reference.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <span>

extern "C" void uml_arm64_gadget_interrupt();

namespace {
namespace a = uml::cpu::arm64;
namespace g = a::gadgets;
namespace reference = arm64_gadget_dependency_reference;
constexpr std::uint64_t pc = 0x4000;
constexpr std::uint32_t svc = 0xd400'0001U;

using uml::testing::require;

std::uint32_t move(unsigned destination, unsigned source)
{
	return 0x4ea0'1c00U | (source << 16) | (source << 5) | destination;
}

a::CpuState initial(unsigned seed, std::uint32_t fpcr)
{
	a::CpuState cpu{};
	cpu.pc = pc;
	cpu.fpcr = fpcr;
	cpu.fpsr = 0x0800'0001U;
	cpu.nzcv = 0xa000'0000U;
	cpu.sp = 0x8000;
	for (unsigned reg = 0; reg < 32; ++reg) {
		// Mix finite values, special FP payloads, and integer limits. Inactive
		// lanes remain nonzero so narrow writes are observable.
		for (unsigned lane = 0; lane < 4; ++lane) {
			std::uint32_t bits = 0x3e80'0001U +
				((reg * 3 + lane + seed) % 16) * 0x0008'0101U;
			constexpr std::array special{0U, 0x8000'0000U, 0x7f80'0000U, 0xff80'0000U,
				0x7fc1'2345U, 0x7f81'2345U, 1U, 0x8000'0001U,
				0xffff'ffffU, 0x7fff'ffffU, 0x0000'ffffU, 0x8000'ffffU};
			if (seed % 4 != 0)
				bits = special[((seed % 4 - 1) * 4) + ((reg + lane) % 4)];
			for (unsigned byte = 0; byte < 4; ++byte)
				cpu.vectors[reg].bytes[lane * 4 + byte] = std::byte(bits >> (byte * 8));
		}
		if (reg < cpu.registers.size()) cpu.registers[reg] = reg * 12345 + seed;
	}
	return cpu;
}

bool equal(const a::CpuState &left, const a::CpuState &right)
{
	if (left.registers != right.registers || left.sp != right.sp ||
		left.pc != right.pc || left.nzcv != right.nzcv ||
		left.fpcr != right.fpcr || left.fpsr != right.fpsr) return false;
	for (unsigned i = 0; i < left.vectors.size(); ++i)
		if (left.vectors[i].bytes != right.vectors[i].bytes) return false;
	return true;
}

/*
 * Runs the program with vector forwarding off and on. A modelled program must
 * match the test-local reference; any other program must reproduce the
 * forwarding-off result exactly.
 */
void compare(std::span<const std::uint32_t> code, unsigned seed, std::uint32_t fpcr,
	uml::memory::Tlb &tlb, bool modelled = true)
{
	const auto disabled = g::Builder{}.build(pc, code, {},
		std::pmr::get_default_resource(), {}, {.vector_forwarding = false});
	const auto enabled = g::Builder{}.build(pc, code);
	require(disabled.complete && enabled.complete);
	auto expected = initial(seed, fpcr), off = expected, on = expected;
	if (modelled) for (auto word : code) {
		if (reference::execute(expected, word) !=
			(word == svc ? reference::Step::system_call : reference::Step::continued)) {
			std::fprintf(stderr, "vector reference rejected word=%08x fpcr=%08x\n",
				word, fpcr);
			std::abort();
		}
	}
	for (const auto &[program, state] : {
		std::pair{&disabled.program, &off}, std::pair{&enabled.program, &on}}) {
		std::size_t count{};
		require(g::execute(*program, *state, tlb, count) == uml::cpu::EngineEvent::system_call);
		if (!modelled && state == &off) expected = off;
		if (count != code.size() || !equal(*state, expected)) {
			std::fprintf(stderr, "vector mismatch word=%08x fpcr=%08x forwarding=%d "
				"fpsr=%08x expected=%08x count=%zu\n",
				code[1], fpcr, state == &on, state->fpsr, expected.fpsr, count);
			for (unsigned i = 0; i < 32; ++i)
				if (state->vectors[i].bytes != expected.vectors[i].bytes)
					std::fprintf(stderr, " vector %u differs\n", i);
			std::abort();
		}
	}
}

g::ScalarAccess access(std::uint32_t instruction)
{
	const auto built = g::Builder{}.build(pc, std::array{instruction}, {},
		std::pmr::get_default_resource(), {}, {.vector_forwarding = false});
	for (unsigned words = 2; words <= 4 && words <= built.program.words().size(); ++words) {
		const auto result = g::vector_access(built.program.words().first(words));
		if (result.contract) return result;
	}
	return {};
}

void catalog()
{
	std::set<std::uintptr_t> pointers;
	std::size_t variants{};
	for (const auto &entry : g::vector_contracts()) {
		require(pointers.insert(entry.entries[0]).second && entry.reserved == 0 && entry.family == 0);
		require(entry.operand_words >= 1 && entry.operand_words <= 2);
		[[maybe_unused]] unsigned present{};
		for (unsigned i = 0; i < 3; ++i)
			if (entry.sources[i] != g::scalar_no_register) present |= 1U << i;
		for (unsigned mask = 1; mask < 8; ++mask) {
			require(bool(entry.entries[mask]) == ((mask & present) == mask));
			if (entry.entries[mask]) {
				require(pointers.insert(entry.entries[mask]).second);
				++variants;
			}
		}
	}
	// A shared ordinary entry would need a combined variant contract, not
	// two independent pointer substitutions by the builder.
	for (const auto &entry : g::scalar_contracts())
		require(!pointers.contains(entry.entries[0]));
	require(g::vector_contracts().size() >= 500);
	std::printf("vector catalog: %zu ordinary entries, %zu source-mask variants\n",
		g::vector_contracts().size(), variants);
}

void broad_matrix(uml::memory::Tlb &tlb)
{
	// Enumerate encoding controls and retain one representative of each entry
	// the production builder selects. The matrix spans the whole vector
	// catalog, beyond the test-local reference, so forwarding must reproduce
	// the forwarding-off result; arm64-instruction-corpus owns each entry's
	// architectural result.
	std::map<std::uintptr_t, std::uint32_t> representatives;
	for (unsigned prefix : {0x0eU, 0x2eU, 0x4eU, 0x6eU, 0x5eU, 0x7eU,
		0x0fU, 0x2fU, 0x4fU, 0x6fU, 0x5fU, 0x7fU, 0x1eU})
	for (unsigned control = 0; control < 16; ++control)
	for (unsigned opcode = 0; opcode < 64; ++opcode) {
		const auto word = (prefix << 24) | (control << 20) | (2U << 16) |
			(opcode << 10) | (1U << 5) | 3U;
		const auto selected = access(word);
		if (selected.contract)
			representatives.emplace(selected.contract->entries[0], word);
	}
	std::size_t comparisons{}, edges{};
	unsigned observed_masks{};
	for (const auto &[entry, original] : representatives)
	for (unsigned pattern = 0; pattern < 8; ++pattern) {
		auto word = original;
		if (pattern & 1) word &= ~(31U << 5);
		if (pattern & 2) word &= ~(31U << 16);
		if (pattern & 4) word &= ~31U;
		const auto selected = access(word);
		// Some by-element register bits encode an index; never silently turn
		// an alias test into a different specialization.
		if (!selected.contract || selected.contract->entries[0] != entry) continue;
		require(selected.destination == UML_GADGET_CPU_V0 + (word & 31U) * 16U);
		const auto mask = selected.source_mask(UML_GADGET_CPU_V0);
		observed_masks |= 1U << mask;
		edges += mask != 0;
		const std::array code{move(0, 31), word, move(30, word & 31U), svc};
		const auto on = g::Builder{}.build(pc, code);
		const auto off = g::Builder{}.build(pc, code, {}, std::pmr::get_default_resource(), {},
			{.vector_forwarding = false});
		constexpr auto position = 2U;
		require(on.program.words().size() == off.program.words().size());
		require(off.program.words()[position] == entry);
		require(on.program.words()[position] == selected.contract->entries[mask]);
		const auto consumer = access(code[2]);
		require(consumer.contract != nullptr);
		const auto consumer_mask = consumer.source_mask(selected.destination);
		require(consumer_mask != 0);
		require(on.program.words()[position + 1U + selected.contract->operand_words] ==
			consumer.contract->entries[consumer_mask]);
		for (auto fpcr : {0U, 4U, 3U, 7U, 0x0340'0000U}) {
			compare(code, static_cast<unsigned>(comparisons * 4), fpcr, tlb, false);
			for (unsigned payload = 1; payload < 4; ++payload)
				compare(code, payload, fpcr, tlb, false);
			++comparisons;
		}
	}
	require(representatives.size() >= 500 && edges >= 500 && observed_masks == 255);
	std::printf("vector matrix: %zu selected entries, %zu dependent cases, "
		"%zu finite and %zu exceptional-payload off/on comparisons, "
		"all source masks\n",
		representatives.size(), edges, comparisons, comparisons * 3);
}

void boundaries(uml::memory::Tlb &tlb)
{
	for (unsigned reg = 0; reg < 32; ++reg) {
		// V31 is a real vector, unlike scalar ZR/SP encodings. Exercise the
		// full register-index range, including packed offsets above 0xff.
		const std::array code{move(reg, (reg + 1) % 32),
			move((reg + 2) % 32, reg), move((reg + 3) % 32, (reg + 2) % 32), svc};
		compare(code, reg * 4, 0, tlb);
	}
	const auto producer = move(0, 31), consumer = move(1, 0);
	for (const auto middle : {
		0x9100'0400U, // Scalar arithmetic must break vector adjacency.
		0xd503'201fU, // Undeclared NOP.
		0x6f00'e400U, // MOVI overwrites V0 outside the vector catalog.
		0x0e01'3c00U, // UMOV w0,v0.b[0], outside the vector catalog.
	}) {
		const std::array code{producer, middle, consumer, svc};
		compare(code, 124, 0, tlb);
		const auto on = g::Builder{}.build(pc, code);
		const auto off = g::Builder{}.build(pc, code, {}, std::pmr::get_default_resource(), {},
			{.vector_forwarding = false});
		require(std::equal(on.program.words().begin(), on.program.words().end(),
			off.program.words().begin(), off.program.words().end()));
	}
}

void linked_boundaries(uml::memory::Tlb &tlb)
{
	constexpr unsigned iterations = 17;
	const std::array code{0x4e21'8400U, move(2, 0), 0xf100'0463U, 0x54ff'ffa1U};
	auto expected = initial(0, 0);
	expected.registers[3] = iterations;
	for (unsigned i = 0; i < iterations; ++i)
		for (auto word : code)
			require(reference::execute(expected, word) == reference::Step::continued);
	for (bool forwarding : {false, true}) {
		std::array<std::uintptr_t, 2> terminal{
			reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_interrupt), pc + 16};
		g::BlockLink loop{.guest_pc = pc};
		g::BlockLink end{.stream = terminal.data(), .instruction_count = 0, .guest_pc = pc + 16};
		const auto built = g::Builder{}.build(pc, code,
			[&](std::uint64_t target) -> const g::BlockLink * {
				if (target == pc) return &loop;
				require(target == pc + 16);
				return &end;
			}, std::pmr::get_default_resource(), {}, {.vector_forwarding = forwarding});
		require(built.complete && built.program.instruction_count() == code.size());
		loop.stream = built.program.words().data();
		loop.instruction_count = code.size();
		auto state = initial(0, 0);
		state.registers[3] = iterations;
		std::size_t count{};
		require(g::execute(built.program, state, tlb, count) == uml::cpu::EngineEvent::interrupt);
		require(count == iterations * code.size() && equal(state, expected));
	}
}
} // namespace

void run_gadget_vector_dependency_tests()
{
	const uml::cpu::HostCapabilitiesScope host{uml::platform::apple::probed_host_capabilities()};
	auto tlb = std::make_unique<uml::memory::Tlb>();
	catalog();
	broad_matrix(*tlb);
	boundaries(*tlb);
	linked_boundaries(*tlb);
	std::puts("vector forwarding boundaries passed");
}
