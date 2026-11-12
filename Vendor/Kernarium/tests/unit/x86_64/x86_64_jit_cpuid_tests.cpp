// SPDX-License-Identifier: GPL-2.0-or-later
/* CPUID through the x86-64 JIT. FEXCore runs CPUID in line and asks the
 * embedder for each answer, which the engine takes from cpuid.hpp, the table
 * the gadget engine's CPUID is tested against. A guest must see the same
 * answers whichever engine runs it, so this runs CPUID for every leaf and
 * subleaf the table answers, the subleaf above each enumerating leaf and a
 * leaf past the maximum, and compares all four registers with the table.
 * AVX-VNNI is leaf 7 subleaf 1 EAX bit 4, with subleaf 1 as the highest in
 * leaf 7 subleaf 0 EAX. */
#define UML_X86_64_SSE_LOOP_JIT 1
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/cpuid.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "cpu/x86_64/segments.hpp"
#include "platform/apple/host_features.hpp"
#include "x86_64_flat_memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace {

namespace x86_64 = uml::cpu::x86_64;
using uml::test::x86_64_flat_memory::FlatMemory;

constexpr std::uint64_t stack_top = 0x3ff0;

int failures;

/* "mov $leaf, %eax; mov $subleaf, %ecx; cpuid; mov %ecx, %esi; syscall":
 * SYSCALL overwrites RCX, so ECX is copied to ESI first. */
x86_64::CpuidResult run_cpuid(std::uint32_t leaf, std::uint32_t subleaf,
	bool &finished)
{
	FlatMemory memory;
	const auto word = [](std::uint32_t value) {
		return std::array<std::uint8_t, 4>{
			static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16),
			static_cast<std::uint8_t>(value >> 24)};
	};
	const auto eax = word(leaf);
	const auto ecx = word(subleaf);
	memory.load(0, {0xb8, eax[0], eax[1], eax[2], eax[3], 0xb9, ecx[0], ecx[1],
		ecx[2], ecx[3], 0x0f, 0xa2, 0x89, 0xce, 0x0f, 0x05});

	x86_64::jit::FexEngine engine{uml::platform::apple::probed_host_capabilities()};
	engine.apply_changes(1, x86_64::ExecutionMode::long64,
		{.mapping_sequence = 1, .code_sequence = 1}, false);
	x86_64::CpuState cpu{};
	cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
	(void)x86_64::enter_segments(cpu);
	cpu.rip = 0;
	cpu.rflags = 0x202;
	cpu.gpr(x86_64::Register::rsp) = stack_top;
	uml::cpu::InterruptToken interrupt{};
	finished = false;
	/* CPUID ends its run, so the program takes more than one. */
	for (unsigned runs = 0; runs < 8; ++runs) {
		const auto result = engine.run(cpu, memory, {
			.generation = 1,
			.instruction_budget = 1000,
			.execution_context = nullptr,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		if (result.execution.reason == uml::cpu::ExitReason::system_call) {
			finished = true;
			break;
		}
		if (result.execution.reason != uml::cpu::ExitReason::instruction_budget)
			break;
	}
	const auto low = [&](x86_64::Register reg) {
		return static_cast<std::uint32_t>(cpu.gpr(reg));
	};
	return {low(x86_64::Register::rax), low(x86_64::Register::rbx),
		low(x86_64::Register::rsi), low(x86_64::Register::rdx)};
}

void expect_leaf(std::uint32_t leaf, std::uint32_t subleaf)
{
	bool finished = false;
	const auto got = run_cpuid(leaf, subleaf, finished);
	const auto want = x86_64::cpuid(leaf, subleaf);
	const bool same = finished && got.eax == want.eax && got.ebx == want.ebx &&
		got.ecx == want.ecx && got.edx == want.edx;
	std::printf("%s leaf 0x%08x subleaf %u: %08x %08x %08x %08x\n",
		same ? "ok" : "FAIL", leaf, subleaf, got.eax, got.ebx, got.ecx, got.edx);
	if (!same) {
		std::printf("  want %08x %08x %08x %08x%s\n", want.eax, want.ebx,
			want.ecx, want.edx, finished ? "" : " (the program did not finish)");
		++failures;
	}
}

} // namespace

int main()
{
	if (!x86_64::jit::fex_engine_available(
		uml::platform::apple::probed_host_capabilities())) {
		std::puts("SKIP the x86-64 JIT cannot run on this host");
		return 0;
	}
	/* Every leaf the table answers, the subleaves of the enumerating ones
	 * and the first leaf above each maximum. */
	static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 18>
		probed{{
			{0U, 0U}, {1U, 0U}, {7U, 0U}, {7U, 1U}, {7U, 2U},
			{13U, 0U}, {13U, 1U}, {13U, 2U}, {13U, 3U}, {14U, 0U},
			{0x8000'0000U, 0U}, {0x8000'0001U, 0U}, {0x8000'0002U, 0U},
			{0x8000'0003U, 0U}, {0x8000'0004U, 0U}, {0x8000'0005U, 0U},
			{0x8000'0008U, 0U}, {0x8000'0009U, 0U},
		}};
	for (const auto &[leaf, subleaf] : probed)
		expect_leaf(leaf, subleaf);

	bool finished = false;
	const auto highest = run_cpuid(7, 0, finished);
	const auto vnni = run_cpuid(7, 1, finished);
	const bool advertised = finished && highest.eax >= 1 && (vnni.eax & 0x10U) != 0;
	std::printf("%s AVX-VNNI: leaf 7 subleaf 0 EAX %08x, subleaf 1 EAX %08x\n",
		advertised ? "ok" : "FAIL", highest.eax, vnni.eax);
	failures += advertised ? 0 : 1;
	std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
