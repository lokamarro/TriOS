// SPDX-License-Identifier: GPL-2.0-or-later
// Manual zero-helper-exit scalar ceiling control. Also builds native-only on
// AArch64 Linux, so the very same instruction bodies can run in a real guest.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

#if __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif
#ifndef UML_SCALAR_CEILING_GADGETS
#define UML_SCALAR_CEILING_GADGETS 0
#endif
#if UML_SCALAR_CEILING_GADGETS
#include "cpu/arm64/gadgets/program.hpp"
namespace a = uml::cpu::arm64;
namespace g = a::gadgets;
extern "C" void uml_arm64_gadget_interrupt();
#endif

namespace {
using Registers = std::array<std::uint64_t, 6>;
using Native = std::uint64_t (*)(std::uint64_t, std::uint64_t *);
#define KERNEL(name) \
 extern "C" std::uint64_t scalar_ceiling_native_##name(std::uint64_t, std::uint64_t *) asm("scalar_ceiling_native_" #name); \
 extern "C" const std::uint32_t scalar_ceiling_body_##name[] asm("scalar_ceiling_body_" #name); \
 extern "C" const std::uint32_t scalar_ceiling_end_##name[] asm("scalar_ceiling_end_" #name);
KERNEL(add_chain) KERNEL(mixed_chain) KERNEL(mixed_twochain)
KERNEL(mixed_sparse) KERNEL(mixed_both) KERNEL(add_independent) KERNEL(varied_chain)
#undef KERNEL
struct Description { const char *name; Native native; const std::uint32_t *begin, *end; };
#define DESCRIPTION(name) Description{#name, scalar_ceiling_native_##name, scalar_ceiling_body_##name, scalar_ceiling_end_##name}
constexpr std::array descriptions{
	DESCRIPTION(add_chain), DESCRIPTION(mixed_chain), DESCRIPTION(mixed_twochain),
	DESCRIPTION(mixed_sparse), DESCRIPTION(mixed_both), DESCRIPTION(add_independent),
	DESCRIPTION(varied_chain)};
#undef DESCRIPTION

void require(bool ok, const char *message)
{
	if (ok) return;
	std::fprintf(stderr, "scalar ceiling: %s\n", message);
	std::exit(1);
}
std::uint64_t checksum(const Registers &values)
{
	std::uint64_t result{};
	for (auto value : values) result ^= value;
	return result;
}
std::uint64_t now()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

#if UML_SCALAR_CEILING_GADGETS
struct Workload final {
	static constexpr std::uint64_t pc = 0x1000;
	std::array<std::uintptr_t, 2> terminal{
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_interrupt), pc + 72};
	g::BlockLink loop{.guest_pc = pc};
	g::BlockLink end{.stream = terminal.data(), .instruction_count = 0, .guest_pc = pc + 72};
	std::array<std::optional<g::Program>, 2> variants;
	std::optional<g::Program> program;
	alignas(128) a::CpuState cpu;
	uml::memory::Tlb tlb;
	std::size_t changed_words{};
	Workload(const Description &description)
	{
		require(reinterpret_cast<std::uintptr_t>(description.end) -
			reinterpret_cast<std::uintptr_t>(description.begin) == 72, "unexpected body length");
		std::array<std::uint32_t, 18> instructions{};
		std::memcpy(instructions.data(), description.begin, sizeof(instructions));
		for (bool forwarding : {false, true}) {
			auto result = g::Builder{}.build(pc, instructions,
				[this](std::uint64_t target) -> const g::BlockLink * {
					if (target == pc) return &loop;
					require(target == pc + 72, "unexpected branch target");
					return &end;
				}, std::pmr::get_default_resource(), {}, {.scalar_forwarding = forwarding});
			require(result.complete && result.program.instruction_count() == 18, "incomplete translation");
			variants[forwarding].emplace(std::move(result.program));
		}
		const auto off = variants[0]->words(), on = variants[1]->words();
		require(off.size() == on.size(), "different stream sizes");
		for (std::size_t i = 0; i < off.size(); ++i) changed_words += off[i] != on[i];
		if (std::string_view{description.name} == "add_independent" ||
			std::string_view{description.name} == "mixed_twochain")
			require(changed_words == 0, "independent control has a changed stream");
		program.emplace(*variants[0]);
		loop.stream = program->words().data();
		// Suppress loop retirement/budget replenishment for this manual ideal
		// control only. The full program is entered once and exits once.
		loop.instruction_count = 0;
	}
	void select(bool forwarding)
	{
		// ProgramWords copy assignment reuses the existing allocation. Only
		// stream data changes, before timing; CPU, TLB and link addresses match.
		*program = *variants[forwarding];
		require(program->words().data() == loop.stream, "stream allocation moved");
	}
	a::CpuState run(std::uint64_t iterations, const Registers &input)
	{
		cpu = {};
		cpu.registers[19] = iterations;
		std::copy(input.begin(), input.end(), cpu.registers.begin() + 20);
		std::size_t retired{};
		require(g::execute(*program, cpu, tlb, retired) == uml::cpu::EngineEvent::interrupt &&
			retired == 18 && cpu.registers[19] == 0, "no-exit sentinel failed");
		return cpu;
	}
};
Registers values(const a::CpuState &cpu)
{
	Registers result{};
	std::copy_n(cpu.registers.begin() + 20, result.size(), result.begin());
	return result;
}
#endif
} // namespace

int main(int argc, char **argv)
{
	if (argc > 1 && std::string_view{argv[1]} == "--help") {
		std::puts("scalar-ceiling [iterations=50000000] [repetitions=6] [case=all]\n"
			"16 useful instructions per iteration; alternates native/off/on order.\n"
			"Build without UML_SCALAR_CEILING_GADGETS for native-only AArch64 Linux.\n"
			"Diagnostic counters must be disabled. Results are a ceiling, not an application score.");
		return 0;
	}
	const auto iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 50'000'000ULL;
	const auto repetitions = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 6UL;
	const std::string_view selection = argc > 3 ? argv[3] : "all";
	require(iterations > 0 && repetitions > 0 && iterations <= (1ULL << 50), "invalid iteration count");
#if __APPLE__
	require(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0, "performance QoS");
#endif
	std::uint64_t seed = 0xc9fb'8293'91de'dabeULL;
	bool selected{};
	for (const auto &description : descriptions) {
		if (selection != "all" && selection != description.name) continue;
		selected = true;
#if UML_SCALAR_CEILING_GADGETS
		auto workload = std::make_unique<Workload>(description);
#endif
		for (unsigned trial = 0; trial < 12; ++trial) {
			Registers input{};
			for (auto &value : input) {
				seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; value = seed;
			}
			for (std::uint64_t count : {1, 2, 3, 17, 101}) {
				auto reference = input;
				require(description.native(count, reference.data()) == checksum(reference), "native checksum");
#if UML_SCALAR_CEILING_GADGETS
				workload->select(false);
				const auto before = workload->run(count, input);
				workload->select(true);
				const auto after = workload->run(count, input);
				require(before.registers == after.registers && before.sp == after.sp &&
					before.nzcv == after.nzcv && before.pc == after.pc, "forwarding state mismatch");
				require(values(before) == reference, "native/gadget register mismatch");
#endif
			}
		}
		const Registers initial{1, 3, 5, 7, 11, 13};
		auto expected = initial;
		description.native(iterations, expected.data());
#if UML_SCALAR_CEILING_GADGETS
		workload->select(false); workload->run(1'000'000, initial);
		workload->select(true); workload->run(1'000'000, initial);
		std::printf("CONTROL case=%s stream_word_changes=%zu shared_execution_addresses=1\n",
			description.name, workload->changed_words);
#endif
		std::printf("CONFIG case=%s useful_ops=16 correctness=60_seeds_counts gadget_build=%d\n",
			description.name, UML_SCALAR_CEILING_GADGETS);
		for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
			constexpr unsigned modes = UML_SCALAR_CEILING_GADGETS ? 3 : 1;
			for (unsigned step = 0; step < modes; ++step) {
				const auto mode = repetition % 2 ? modes - 1 - step : step;
#if UML_SCALAR_CEILING_GADGETS
				if (mode != 0) workload->select(mode == 2);
#endif
				const auto start = now();
				std::uint64_t elapsed{};
				if (mode == 0) {
					auto result = initial;
					const auto actual = description.native(iterations, result.data());
					elapsed = now() - start;
					require(result == expected && actual == checksum(expected), "timed native result");
				}
#if UML_SCALAR_CEILING_GADGETS
				else {
					const auto actual = workload->run(iterations, initial);
					elapsed = now() - start;
					require(values(actual) == expected, "timed gadget result");
				}
#endif
				std::printf("SAMPLE case=%s rep=%u mode=%s iterations=%llu ns=%llu gops=%.9f\n",
					description.name, repetition, mode == 0 ? "native" : mode == 1 ? "off" : "on",
					static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(elapsed),
					double(iterations) * 16.0 / double(elapsed));
				std::fflush(stdout);
			}
		}
	}
	require(selected, "unknown case");
}
