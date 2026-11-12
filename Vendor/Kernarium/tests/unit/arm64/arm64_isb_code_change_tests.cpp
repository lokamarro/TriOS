// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * An ISB is a context synchronization point for code another vCPU changed.
 * Once the other vCPU has written an instruction and synchronized it, as the
 * kernel's IC IVAU path does (darwin_uml_static_vm_invalidate_code), and this
 * vCPU has observed that through memory and run an ISB, this vCPU runs the
 * new instruction, whatever translations and links it holds.
 *
 * Each case runs a guest that, round after round, announces the round,
 * waits for the other vCPU's go, runs an ISB and then the instruction the
 * other vCPU rewrote for that round, counting each round in which it saw an
 * older instruction. The other vCPU is a second thread. The guest stays in
 * translated code while it waits, so without the ISB's check it runs the
 * instruction it translated in an earlier round. Both engines, in A64 and in
 * A32 with the ARMv8 ISB and the CP15 ISB.
 */
#include "jit_test_support.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"
#include "scratch_backing.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::emulator::tests::GenerationMapping;
using uml::testing::require;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
/* Below 4 GiB, so the AArch32 cases can use them too. */
constexpr std::uint32_t code_base = 0x4f00'0000U;
constexpr std::uint32_t data_base = 0x5100'0000U;
/* The data page: the other vCPU's go, then the guest's round. */
constexpr std::uint32_t go_offset = 0;
constexpr std::uint32_t ready_offset = 4;
constexpr std::uint32_t rounds = 64;
constexpr auto deadline = std::chrono::seconds{60};

enum class Guest { a64, a32, a32_cp15 };

/* The guest's program and where in it the instruction the other vCPU
 * rewrites lies, as a function of the round. */
struct Program final {
	std::vector<std::uint32_t> words;
	std::uint32_t patch_offset{};
	std::uint32_t (*patch)(std::uint32_t round){};
};

[[nodiscard]] std::uint32_t a64_branch_ne(std::size_t from, std::size_t to)
{
	const auto offset = (static_cast<std::int64_t>(to) -
		static_cast<std::int64_t>(from)) / 4;
	return 0x5400'0001U |
		(static_cast<std::uint32_t>(offset) & 0x7'ffffU) << 5;
}

[[nodiscard]] std::uint32_t a32_branch_ne(std::size_t from, std::size_t to)
{
	const auto offset = (static_cast<std::int64_t>(to) -
		static_cast<std::int64_t>(from) - 8) / 4;
	return 0x1a00'0000U | (static_cast<std::uint32_t>(offset) & 0xff'ffffU);
}

/* x2/r2 holds the go word's address, x3/r3 the round's, x5/r5 counts the
 * rounds and x6/r6 the stale ones. */
[[nodiscard]] Program program(Guest guest)
{
	Program result;
	auto &w = result.words;
	if (guest == Guest::a64) {
		w.push_back(0x5280'0025U);		// movz w5, #1
		const auto top = w.size() * 4;
		w.push_back(0x889f'fc65U);		// stlr w5, [x3]
		const auto spin = w.size() * 4;
		w.push_back(0x88df'fc41U);		// ldar w1, [x2]
		w.push_back(0x6b05'003fU);		// cmp w1, w5
		w.push_back(a64_branch_ne(w.size() * 4, spin));
		w.push_back(0xd503'3fdfU);		// isb
		result.patch_offset = static_cast<std::uint32_t>(w.size() * 4);
		result.patch = [](std::uint32_t round) {
			return 0xd280'0000U | round << 5; // movz x0, #round
		};
		w.push_back(result.patch(0));
		w.push_back(0xeb05'001fU);		// cmp x0, x5
		w.push_back(0x9a86'04c6U);		// cinc x6, x6, ne
		w.push_back(0x1100'04a5U);		// add w5, w5, #1
		w.push_back(0x7100'00bfU | (rounds + 1) << 10); // cmp w5, #n
		w.push_back(a64_branch_ne(w.size() * 4, top));
		w.push_back(0xd400'0001U);		// svc #0
		return result;
	}
	w.push_back(0xe3a0'5001U);			// mov r5, #1
	const auto top = w.size() * 4;
	w.push_back(0xe183'fc95U);			// stl r5, [r3]
	const auto spin = w.size() * 4;
	w.push_back(0xe192'1c9fU);			// lda r1, [r2]
	w.push_back(0xe151'0005U);			// cmp r1, r5
	w.push_back(a32_branch_ne(w.size() * 4, spin));
	w.push_back(guest == Guest::a32 ? 0xf57f'f06fU : // isb sy
		0xee07'0f95U);			// mcr p15, 0, r0, c7, c5, 4
	result.patch_offset = static_cast<std::uint32_t>(w.size() * 4);
	result.patch = [](std::uint32_t round) {
		return 0xe300'0000U | (round >> 12) << 16 | (round & 0xfffU);
	};						// movw r0, #round
	w.push_back(result.patch(0));
	w.push_back(0xe150'0005U);			// cmp r0, r5
	w.push_back(0x1286'6001U);			// addne r6, r6, #1
	w.push_back(0xe285'5001U);			// add r5, r5, #1
	w.push_back(0xe355'0000U | (rounds + 1));	// cmp r5, #n
	w.push_back(a32_branch_ne(w.size() * 4, top));
	w.push_back(0xef00'0000U);			// svc #0
	return result;
}

[[nodiscard]] const char *name(Guest guest) noexcept
{
	switch (guest) {
	case Guest::a64:
		return "A64 ISB";
	case Guest::a32:
		return "A32 ISB";
	case Guest::a32_cp15:
		return "A32 CP15 ISB";
	}
	return "?";
}

[[noreturn]] void failed(std::string_view engine, Guest guest,
	const char *what, unsigned long long value)
{
	std::fprintf(stderr, "%.*s, %s: %s (%llu)\n",
		static_cast<int>(engine.size()), engine.data(), name(guest),
		what, value);
	std::abort();
}

void run_case(uml::cpu::EngineKind kind, Guest guest, unsigned long generation,
	std::string_view label)
{
	require(darwin_uml_static_vm_activate(generation) == 0);
	{
		const auto built = program(guest);
		uml::testing::ScratchBacking code{"uml-isb-code.XXXXXX", page};
		uml::testing::ScratchBacking data{"uml-isb-data.XXXXXX", page};
		code.put(0, std::as_bytes(std::span{built.words}));
		const GenerationMapping code_mapping{generation, code_base, code, 1,
			PROT_READ | PROT_EXEC, true};
		const GenerationMapping data_mapping{generation, data_base, data, 1,
			PROT_READ | PROT_WRITE, false};
		void *host = nullptr;
		require(darwin_uml_static_vm_translate(generation, data_base,
			DARWIN_UML_STATIC_WRITE, &host) ==
			DARWIN_UML_STATIC_TRANSLATED);
		auto *words = static_cast<std::uint32_t *>(host);
		std::atomic_ref<std::uint32_t> go{words[go_offset / 4]};
		std::atomic_ref<std::uint32_t> ready{words[ready_offset / 4]};

		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr);
		arm64::CpuState cpu{};
		cpu.registers[2] = data_base + go_offset;
		cpu.registers[3] = data_base + ready_offset;
		if (guest == Guest::a64) {
			cpu.pc = code_base;
		} else {
			cpu.a32 = 1;
			cpu.a32_cp15ben = 1;
			cpu.pc = arm64::aarch32::a32_key(code_base);
		}

		/* The other vCPU: rewrite the instruction for the round the
		 * guest announced, synchronize it, then let the guest go. */
		std::atomic_bool stop{false};
		std::atomic_uint32_t other_failed{0};
		std::thread other{[&] {
			const auto until = std::chrono::steady_clock::now() + deadline;
			for (std::uint32_t round = 1; round <= rounds; ++round) {
				while (ready.load(std::memory_order_acquire) != round) {
					if (stop.load(std::memory_order_relaxed) ||
					    std::chrono::steady_clock::now() > until) {
						other_failed.store(round);
						return;
					}
				}
				code.put32(built.patch_offset, built.patch(round));
				if (darwin_uml_static_vm_invalidate_code_generation(
					    generation, code_base + built.patch_offset,
					    sizeof(std::uint32_t)) != 0) {
					other_failed.store(round);
					return;
				}
				go.store(round, std::memory_order_release);
			}
		}};

		const auto until = std::chrono::steady_clock::now() + deadline;
		uml::cpu::ExecutionResult result{};
		for (;;) {
			uml::memory::StaticGuestMemory memory;
			memory.prepare(generation,
				darwin_uml_static_vm_sequences(generation)
					.mapping_changes);
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = std::size_t{1} << 20,
			};
			result = guest == Guest::a64 ? engine->run(request) :
				engine->run_aarch32(request);
			if (result.reason != uml::cpu::ExitReason::instruction_budget &&
			    result.reason != uml::cpu::ExitReason::interrupt)
				break;
			if (std::chrono::steady_clock::now() > until ||
			    other_failed.load() != 0)
				break;
		}
		stop.store(true);
		other.join();
		if (other_failed.load() != 0)
			failed(label, guest, "the other vCPU gave up in round",
				other_failed.load());
		if (result.reason != uml::cpu::ExitReason::system_call)
			failed(label, guest, "the guest did not finish; exit reason",
				static_cast<unsigned long long>(result.reason));
		if ((cpu.registers[5] & 0xffff'ffffU) != rounds + 1)
			failed(label, guest, "the guest finished in round",
				cpu.registers[5]);
		if (cpu.registers[6] != 0)
			failed(label, guest,
				"rounds that ran a stale instruction after an ISB",
				cpu.registers[6]);
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

/* With an engine name (gadgets or jit), and then a guest (a64, a32 or
 * a32-cp15), only those cases run. */
int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	const std::string_view only_engine = argc > 1 ? argv[1] : "";
	const std::string_view only_guest = argc > 2 ? argv[2] : "";
	unsigned long generation = 0x15b0;
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		     uml::cpu::EngineKind::jit}) {
		const std::string_view engine =
			kind == uml::cpu::EngineKind::jit ? "jit" : "gadgets";
		if (!only_engine.empty() && only_engine != engine)
			continue;
		if (!uml::cpu::cpu_engine_available(kind)) {
			std::printf("%.*s: unavailable on this host; skipped\n",
				static_cast<int>(engine.size()), engine.data());
			continue;
		}
		const std::pair<Guest, std::string_view> guests[] = {
			{Guest::a64, "a64"},
			{Guest::a32, "a32"},
			{Guest::a32_cp15, "a32-cp15"},
		};
		for (const auto &[guest, guest_name] : guests) {
			if (!only_guest.empty() && only_guest != guest_name)
				continue;
			run_case(kind, guest, generation++, engine);
		}
	}
	std::puts("arm64-isb-code-change: passed");
	return 0;
}
