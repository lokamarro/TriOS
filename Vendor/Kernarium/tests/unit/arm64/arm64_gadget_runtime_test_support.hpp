// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the gadget-executor tests that run guest code over a mapped static
 * address space share: one run of an executor, the few instructions their
 * programs are made of, and the storm of vCPUs that follow code rewritten
 * under them. A test brings the address space (JitBacking and JitMapping)
 * and its own scenario. */

#include "jit_test_support.hpp"

#include "cpu/arm64/gadgets/executor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace uml::emulator::tests::gadget_runtime {

using ::uml::cpu::EngineEvent;
using ::uml::cpu::arm64::CpuState;
using ::uml::cpu::arm64::gadgets::BuildOptions;
using ::uml::cpu::arm64::gadgets::GadgetExecutor;
using ::uml::memory::StaticGuestMemory;

inline constexpr std::uint64_t code_base = 0xd0'0000;
inline constexpr std::uint64_t target = code_base + 0x100;
inline constexpr std::uint32_t system_call = 0xd400'0001U;

/* Sharing needs a host that retargets a link pair in one access, which every
 * Apple Silicon host does; the baseline profile unit tests use otherwise
 * leaves it out. */
[[nodiscard]] inline BuildOptions sharing_options()
{
	BuildOptions options;
	options.host_capabilities.supported.set(
		static_cast<std::size_t>(::uml::cpu::ArmFeature::lse2));
	return options;
}

struct Outcome final {
	EngineEvent event;
	std::size_t executed;
};

[[nodiscard]] inline Outcome run_once(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

/* Run from `pc` until the system call, at most `limit` blocks. */
[[nodiscard]] inline std::size_t run_to_system_call(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, CpuState &cpu,
	std::uint64_t pc, std::size_t limit = 4)
{
	cpu = {};
	cpu.pc = pc;
	std::size_t executed = 0;
	for (std::size_t block = 0; block < limit; ++block) {
		const auto outcome = run_once(executor, memory, generation, cpu);
		executed += outcome.executed;
		if (outcome.event == EngineEvent::system_call)
			return executed;
		require(outcome.event == EngineEvent::continued);
	}
	require(false);
	return executed;
}

/* `movz xreg, #value`. */
[[nodiscard]] constexpr std::uint32_t move_immediate(
	unsigned reg, unsigned value) noexcept
{
	return 0xd280'0000U | ((value & 0xffffU) << 5U) | (reg & 31U);
}

[[nodiscard]] inline std::uint32_t branch(std::uint64_t source,
	std::uint64_t to)
{
	const auto displacement = static_cast<std::int64_t>(to) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

/* `mov x0, #first; b target` at code_base and `mov x1, #second; svc` at
 * target. */
inline void put_program(JitBacking &backing, unsigned first, unsigned second)
{
	backing.put32(0, move_immediate(0, first));
	backing.put32(4, branch(code_base + 4, target));
	backing.put32(target - code_base, move_immediate(1, second));
	backing.put32(target - code_base + 4, system_call);
}

[[nodiscard]] constexpr std::size_t pages_for(std::size_t bytes) noexcept
{
	return (bytes + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;
}

/* A vCPU's executor, memory view and CPU state are larger than the stack a
 * sanitizer leaves a test, so tests keep them on the heap. */
struct Vcpu final {
	explicit Vcpu(const BuildOptions &options = sharing_options())
		: executor{options}
	{
	}
	StaticGuestMemory memory;
	GadgetExecutor executor;
	CpuState cpu{};
};

/* Several vCPUs run, link, retire and revalidate the same `spaces` address
 * spaces (generations first_generation onward, each holding put_program's
 * code) while this thread rewrites their code. Every run must see one of the
 * values the code has held, and once the writer stops, the last ones. `after`
 * is called on each vCPU's thread, with its executor, when its runs are over. */
inline void code_change_storm(unsigned long first_generation,
	std::size_t spaces, std::size_t vcpus,
	const std::function<void(GadgetExecutor &)> &after = {})
{
	constexpr unsigned values = 16;
	std::vector<std::unique_ptr<JitBacking>> backings;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (std::size_t space = 0; space < spaces; ++space) {
		backings.push_back(std::make_unique<JitBacking>(1));
		put_program(*backings.back(), 100, 200);
		mappings.push_back(std::make_unique<JitMapping>(
			first_generation + space, code_base, *backings.back(), 1));
	}

	std::atomic<bool> stop{};
	std::atomic<std::size_t> runs{};
	const auto vcpu = [&](std::size_t index) {
		const auto owned = std::make_unique<Vcpu>();
		auto &memory = owned->memory;
		auto &executor = owned->executor;
		auto &cpu = owned->cpu;
		std::size_t round = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			/* Each vCPU walks the address spaces in its own order, so
			 * the eight-space window retires different ones in each. */
			const auto space = (round * (index + 1) + index) % spaces;
			++round;
			(void)run_to_system_call(executor, memory,
				first_generation + space, cpu, code_base);
			require(cpu.registers[0] >= 100 &&
				cpu.registers[0] < 100 + values);
			require(cpu.registers[1] >= 200 &&
				cpu.registers[1] < 200 + values);
			runs.fetch_add(1, std::memory_order_relaxed);
		}
		/* The writer has stopped: every space runs its last code. */
		for (std::size_t space = 0; space < spaces; ++space) {
			(void)run_to_system_call(executor, memory,
				first_generation + space, cpu, code_base);
			require(cpu.registers[0] == 100 + values - 1);
			require(cpu.registers[1] == 200 + values - 1);
		}
		if (after)
			after(executor);
	};
	std::vector<std::thread> threads;
	for (std::size_t index = 0; index < vcpus; ++index)
		threads.emplace_back(vcpu, index);

	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds{20};
	for (unsigned value = 1; value < values; ++value) {
		const auto goal = runs.load() + 5000;
		while (runs.load() < goal &&
		       std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		for (std::size_t space = 0; space < spaces; ++space) {
			put_program(*backings[space], 100 + value, 200 + value);
			require(darwin_uml_static_vm_invalidate_code_generation(
				first_generation + space, code_base,
				target - code_base + 8) == 0);
		}
	}
	const auto goal = runs.load() + 5000;
	while (runs.load() < goal && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	stop.store(true);
	for (auto &thread : threads)
		thread.join();
	require(runs.load() >= values * 5000);
}

} // namespace uml::emulator::tests::gadget_runtime
