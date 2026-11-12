/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "platform/apple/host_features.hpp"
#include "memory/static_guest_memory.hpp"
#include "mach_vm_region_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "uml_darwin_guest.h"
#include "allocation_failure.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <utility>

namespace {

class TranslatedBoundaryExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
	}

	void set_single_step(bool enabled) noexcept
	{
		single_step = enabled;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = block_size;
		return uml::cpu::EngineEvent::interrupt;
	}

	[[nodiscard]] bool preempted() const noexcept
	{
		return false;
	}

	[[nodiscard]] std::uint64_t fault_address() const noexcept
	{
		return 0;
	}

	[[nodiscard]] bool fault_alignment() const noexcept { return false; }

	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}

	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	static constexpr std::size_t block_size = 4096;
	std::size_t begin_calls{};
	std::size_t end_calls{};
	bool single_step{};
	std::size_t block_calls{};
};

class ThrowingTranslatedExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		if (entered)
			return false;
		entered = true;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
		entered = false;
	}

	void set_single_step(bool) noexcept {}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = committed_before_throw;
		if (throw_on_run)
			throw std::runtime_error{"injected translated-engine failure"};
		return uml::cpu::EngineEvent::fatal;
	}

	[[nodiscard]] bool preempted() const noexcept { return false; }
	[[nodiscard]] std::uint64_t fault_address() const noexcept { return 0; }
	[[nodiscard]] bool fault_alignment() const noexcept { return false; }

	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	bool throw_on_run{true};
	bool entered{};
	std::size_t committed_before_throw{};
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t block_calls{};
};

/* An executor that answers every block of either ISA with one scripted
 * event, and reports distinctive fault, syndrome and BKPT values so a test
 * can tell which of them an entry loop forwarded. */
class ScriptedExecutor final {
public:
	static constexpr std::uint64_t fault_location = 0x7000;
	static constexpr std::uint32_t trap_syndrome = 0x1b;
	static constexpr std::uint32_t bkpt_immediate = 0x1234;

	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t pc)
	{
		begin_pc = pc;
		++begin_calls;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
	}

	void set_single_step(bool) noexcept {}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++a64_calls;
		executed = 1;
		return event;
	}

	void enter_aarch32(uml::cpu::arm64::CpuState &) noexcept
	{
		a32_entry_before_block = a32_calls == 0;
		++a32_entries;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_a32_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++a32_calls;
		executed = 1;
		return event;
	}

	[[nodiscard]] bool preempted() const noexcept { return false; }
	[[nodiscard]] std::uint64_t fault_address() const noexcept
	{
		return fault_location;
	}
	[[nodiscard]] bool fault_alignment() const noexcept { return true; }
	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::write;
	}
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return trap_syndrome;
	}
	[[nodiscard]] std::uint32_t breakpoint_immediate() const noexcept
	{
		return bkpt_immediate;
	}

	uml::cpu::EngineEvent event{uml::cpu::EngineEvent::fatal};
	std::uint64_t begin_pc{};
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t a64_calls{};
	std::size_t a32_calls{};
	std::size_t a32_entries{};
	bool a32_entry_before_block{};
};

struct InterruptPollState final {
	std::size_t calls{};
	std::size_t pending_on_call{};
};

[[nodiscard]] bool poll_interrupt(void *opaque) noexcept
{
	auto &state = *static_cast<InterruptPollState *>(opaque);
	++state.calls;
	return state.calls >= state.pending_on_call;
}

using uml::testing::require;

class StaticGeneration final {
public:
	explicit StaticGeneration(unsigned long generation)
		: generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0,
			"cannot activate fault-direction generation");
	}

	~StaticGeneration()
	{
		if (darwin_uml_static_vm_destroy(generation_) != 0)
			std::abort();
	}

	void map(std::uint64_t address,
		uml::emulator::tests::JitBacking &backing, int protection,
		bool guest_code = true)
	{
		require(darwin_uml_static_vm_map_generation(
			generation_, reinterpret_cast<void *>(address),
			backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			protection, guest_code ? 1 : 0,
			(protection & PROT_WRITE) != 0) == 0,
			"cannot map fault-direction page");
	}

	void protect(std::uint64_t address, int protection)
	{
		require(darwin_uml_static_vm_protect(
			reinterpret_cast<void *>(address),
			DARWIN_UML_GUEST_PAGE_SIZE, protection) == 0,
			"cannot update fault-direction protection");
	}

private:
	unsigned long generation_;
};

[[nodiscard]] std::uint64_t native_fpcr() noexcept
{
	std::uint64_t value{};
	__asm__ volatile("mrs %0, fpcr" : "=r"(value));
	return value;
}

struct AllocationBoundary final {
	std::size_t blocks{};
};

[[nodiscard]] bool fail_next_block_allocation(
	uml::cpu::arm64::CpuState &, void *opaque) noexcept
{
	auto &state = *static_cast<AllocationBoundary *>(opaque);
	if (++state.blocks == 1)
		uml::testing::fail_next_allocation();
	return false;
}

void test_factory_contract()
{
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		uml::cpu::EngineKind::jit}) {
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr, "engine factory returned null");
		require(engine->kind() == kind, "engine kind did not survive factory");
		if (kind != uml::cpu::EngineKind::jit)
			require(!uml::testing::process_has_writable_executable_region(),
				"non-JIT engine factory created W+X memory");
	}
	const auto invalid = static_cast<uml::cpu::EngineKind>(99);
	require(!uml::cpu::cpu_engine_available(invalid),
		"invalid engine kind was reported as available");
	require(uml::cpu::make_cpu_engine(invalid) == nullptr,
		"invalid engine kind fell back to a production engine");
}

void test_all_engines_scope_unsupported_instruction_to_guest()
{
	using uml::emulator::tests::JitBacking;
	using uml::emulator::tests::JitMapping;
	constexpr std::uint64_t base = 0x120'0000;
	/* PTRUE P0.B: allocated SVE, outside the profile of a virtual CPU
	 * without SVE. Every adapter must turn it into a precise guest
	 * illegal-instruction exit. Each engine translates for what it
	 * presents, as the runtime configures it.
	 */
	constexpr std::uint32_t unsupported_instruction = 0x2518'e3e0U;
	unsigned long generation = 0x7200;
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind,
			uml::cpu::EngineConfiguration{
				.host_capabilities =
					uml::cpu::engine_host_capabilities(kind,
						uml::cpu::host_capabilities()),
			});
		require(engine != nullptr && engine->kind() == kind,
			"requested engine was replaced by a fallback");
		{
			JitBacking backing{1};
			backing.put32(0, unsupported_instruction);
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 1,
			};
			const auto result = engine->run(request);
			if (result.reason !=
				    uml::cpu::ExitReason::illegal_instruction ||
			    result.retired_instructions != 0 || cpu.pc != base)
				std::fprintf(stderr,
					"unsupported diagnostics engine=%d reason=%d "
					"retired=%zu pc=%#llx\n",
					static_cast<int>(kind),
					static_cast<int>(result.reason),
					result.retired_instructions,
					static_cast<unsigned long long>(cpu.pc));
			require(result.reason ==
					uml::cpu::ExitReason::illegal_instruction &&
				result.retired_instructions == 0 &&
				cpu.pc == base,
				"engine did not preserve precise guest-illegal state");
		}
		++generation;
		{
			JitBacking backing{1};
			backing.put32(0, 0xd280'00a0U); // movz x0, #5
			backing.put32(4, unsupported_instruction);
			backing.put32(8, 0xd400'0001U); // svc #0
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 8,
			};
			const auto result = engine->run(request);
			require(result.reason ==
					uml::cpu::ExitReason::illegal_instruction &&
				result.retired_instructions == 1 &&
				cpu.pc == base + sizeof(std::uint32_t) &&
				cpu.registers[0] == 5,
				"engine lost a supported prefix before an implementation gap");
		}
		++generation;
		{
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U);
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 1,
			};
			const auto result = engine->run(request);
			require(result.reason == uml::cpu::ExitReason::system_call &&
				result.retired_instructions == 1,
				"engine was unusable after a guest implementation gap");
		}
		++generation;
	}
}

void test_all_engines_preserve_fault_direction_and_retry()
{
	using uml::emulator::tests::JitBacking;
	constexpr std::uint64_t base = 0x180'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long generation = 0x7600;
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr && engine->kind() == kind,
			"fault-direction engine was replaced by a fallback");
		const auto run = [&](uml::cpu::arm64::CpuState &cpu,
			uml::memory::StaticGuestMemory &memory,
			unsigned long current_generation) {
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = current_generation,
				.instruction_budget = 8,
			};
			return engine->run(request);
		};

		/* An absent instruction page is an execute fault and does not retire.
		 * Mapping it executable must make the exact same engine reusable. */
		{
			StaticGeneration state{generation};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access ==
					uml::memory::MemoryAccess::execute &&
				cpu.pc == base,
				"unmapped entry fetch lost execute-fault precision");
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U); // svc #0
			state.map(base, backing, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 1,
				"execute-fault mapping retry did not resume precisely");
		}
		++generation;

		/* A mapped but non-executable entry has the same execute direction and
		 * becomes runnable after only execute permission is added. */
		{
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, backing, PROT_READ);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access ==
					uml::memory::MemoryAccess::execute &&
				cpu.pc == base,
				"non-executable entry fetch lost execute direction");
			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 1,
				"execute-permission retry did not resume precisely");
		}
		++generation;

		/* Execute-only code can fetch LDR, but its data read of that exact PC
		 * must remain a read fault. Address equality is not access metadata. */
		{
			JitBacking backing{1};
			backing.put32(0, 0xf940'0020U); // ldr x0, [x1]
			backing.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, backing, PROT_EXEC);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			cpu.registers[1] = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access == uml::memory::MemoryAccess::read &&
				cpu.pc == base,
				"data read at PC was misclassified as execute");
			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 2,
				"read-permission retry did not resume at the faulting load");
		}
		++generation;

		for (const auto [instruction, access] :
		     std::array{
			     std::pair{0xf940'0020U,
				     uml::memory::MemoryAccess::read},
			     std::pair{0xf900'0020U,
				     uml::memory::MemoryAccess::write}}) {
			JitBacking backing{1};
			backing.put32(0, instruction);
			backing.put32(4, 0xd400'0001U);
			StaticGeneration state{generation};
			state.map(base, backing, PROT_READ | PROT_EXEC);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			cpu.registers[1] = base + page;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base + page &&
				fault.fault.access == access && cpu.pc == base,
				"ordinary data fault direction or precision changed");
			++generation;
		}
	}
}

void test_c_bridge_preserves_fault_direction_and_retry()
{
	using EntryPoint = int (*)(void *, void *, unsigned long,
		unsigned long, unsigned long *);
	using uml::emulator::tests::JitBacking;
	struct Engine final {
		uml::cpu::EngineKind kind;
		EntryPoint enter;
	};
	constexpr std::array engines{
		Engine{uml::cpu::EngineKind::gadgets,
			&darwin_uml_static_enter_guest},
		Engine{uml::cpu::EngineKind::jit,
			&darwin_uml_jit_enter_guest},
	};
	constexpr std::uint64_t base = 0x1a0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long generation = 0x7700;

	for (const auto &engine : engines) {
		if (!uml::cpu::cpu_engine_available(engine.kind))
			continue;

		/* Start with readable, non-executable code. After the execute fault,
		 * make it execute-only: the LDR's data address intentionally equals
		 * its PC, proving that the bridge exports direction rather than an
		 * address-based guess. */
		{
			JitBacking code{1};
			code.put32(0, 0xf940'0020U); // ldr x0, [x1]
			code.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, code, PROT_READ);
			darwin_uml_guest_registers registers{};
			uml::testing::GuestFpTail fp{};
			registers.pc = base;
			registers.x[1] = base;
			unsigned long tls = 0;
			const auto execute_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(execute_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base && registers.pc == base &&
				registers.fault.error_code == DARWIN_UML_FAULT_EXECUTE,
				"C bridge lost non-executable fetch direction");

			state.protect(base, PROT_EXEC);
			const auto read_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(read_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base && registers.pc == base &&
				registers.fault.error_code == 0,
				"C bridge misclassified a data read at PC as execute");

			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(retry == DARWIN_UML_EVENT_SYSCALL &&
				registers.pc == base + 8,
				"C bridge read-fault retry was not precise");
		}
		++generation;

		{
			JitBacking code{1};
			JitBacking data{1};
			code.put32(0, 0xf900'0020U); // str x0, [x1]
			code.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, code, PROT_READ | PROT_EXEC);
			state.map(base + page, data, PROT_READ, false);
			darwin_uml_guest_registers registers{};
			uml::testing::GuestFpTail fp{};
			registers.pc = base;
			registers.x[0] = 0x1122'3344'5566'7788ULL;
			registers.x[1] = base + page;
			unsigned long tls = 0;
			const auto write_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(write_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base + page &&
				registers.fault.error_code == DARWIN_UML_FAULT_WRITE &&
				registers.pc == base,
				"C bridge lost write-fault direction or precision");

			state.protect(base + page, PROT_READ | PROT_WRITE);
			const auto retry = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(retry == DARWIN_UML_EVENT_SYSCALL &&
				registers.pc == base + 8,
				"C bridge write-fault retry was not precise");
		}
		++generation;
	}
}

void test_production_translated_exception_contract()
{
	using uml::emulator::tests::JitBacking;
	constexpr std::uint64_t base = 0x1c0'0000;
	constexpr std::uint64_t target = base + 0x10'0000;
	const auto branch_delta = target - (base + sizeof(std::uint32_t));
	const auto branch = 0x1400'0000U |
		(static_cast<std::uint32_t>(branch_delta >> 2U) & 0x03ff'ffffU);
	unsigned long generation = 0x7800;

	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind);
		JitBacking first{1};
		JitBacking second{1};
		first.put32(0, 0x1e22'2820U); // fadd s0, s1, s2
		first.put32(4, branch);        // b target
		// The second block needs a fresh translation, so the injected
		// allocation failure reaches it.
		second.put32(0, 0xd503'3fdfU); // isb
		second.put32(4, 0xd400'0001U); // svc #0
		StaticGeneration state{generation};
		state.map(base, first, PROT_READ | PROT_EXEC);
		state.map(target, second, PROT_READ | PROT_EXEC);
		uml::memory::StaticGuestMemory memory;
		memory.set_generation(generation);
		uml::cpu::arm64::CpuState cpu;
		cpu.pc = base;
		const std::uint32_t one = 0x3f80'0000U;
		std::memcpy(cpu.vectors[1].bytes.data(), &one, sizeof(one));
		std::memcpy(cpu.vectors[2].bytes.data(), &one, sizeof(one));
		const auto caller_fpcr = native_fpcr();
		AllocationBoundary boundary;
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation,
			.instruction_budget = 16,
			.boundary = fail_next_block_allocation,
			.boundary_context = &boundary,
		};
		const auto failed = engine->run(request);
		require(failed.reason == uml::cpu::ExitReason::fatal &&
			failed.retired_instructions == 2 && cpu.pc == target,
			"second-block allocation failure lost its retired prefix");
		require(boundary.blocks == 1 &&
			!uml::testing::cancel_pending_allocation_failure(),
			"second-block allocation injection was not consumed precisely");
		require(!cpu.gadget_fp_active && native_fpcr() == caller_fpcr,
			"fatal translated cleanup leaked the guest host environment");

		request.boundary = nullptr;
		request.boundary_context = nullptr;
		const auto retry = engine->run(request);
		require(retry.reason == uml::cpu::ExitReason::system_call &&
			retry.retired_instructions == 2 && cpu.pc == target + 8,
			"translated engine was not reusable after exception cleanup");
		require(!cpu.gadget_fp_active && native_fpcr() == caller_fpcr,
			"successful retry did not retain balanced host state");
		++generation;
	}
}

void test_gadget_post_execution_failure_preserves_prefix_and_reuse()
{
	using uml::emulator::tests::JitBacking;
	if (!uml::cpu::cpu_engine_available(uml::cpu::EngineKind::gadgets))
		return;

	constexpr std::uint64_t base = 0x1e0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first_target = base + page;
	constexpr std::uint64_t second_target = base + 2 * page;
	constexpr unsigned long generation = 0x7900;
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::gadgets);
	require(engine != nullptr &&
		engine->kind() == uml::cpu::EngineKind::gadgets,
		"gadget post-execution test did not receive the requested engine");

	JitBacking source{1};
	JitBacking first{1};
	JitBacking second{1};
	source.put32(0, 0xd61f'0000U); // indirect transfer through x0
	first.put32(0, 0xd400'0001U);  // svc #0
	second.put32(0, 0xd400'0001U); // svc #0
	StaticGeneration state{generation};
	state.map(base, source, PROT_READ | PROT_EXEC);
	state.map(first_target, first, PROT_READ | PROT_EXEC);
	state.map(second_target, second, PROT_READ | PROT_EXEC);
	uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	uml::cpu::arm64::CpuState cpu;
	cpu.pc = base;
	cpu.registers[0] = first_target;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = 8,
	};
	const auto caller_fpcr = native_fpcr();
	const auto warm = engine->run(request);
	require(warm.reason == uml::cpu::ExitReason::system_call &&
		warm.retired_instructions == 2 && cpu.pc == first_target + 4,
		"gadget indirect-link cache warmup did not reach its boundary");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active &&
		native_fpcr() == caller_fpcr,
		"gadget cache warmup leaked transient execution state");

	/* The source block and its first target are now fully cached. Force the
	 * allocation that learns a different target to fail after the transfer has
	 * already retired and advanced the architectural PC. */
	cpu.pc = base;
	cpu.registers[0] = second_target;
	uml::testing::fail_next_allocation();
	const auto failed = engine->run(request);
	require(failed.reason == uml::cpu::ExitReason::fatal &&
		failed.retired_instructions == 1 && cpu.pc == second_target,
		"post-execution cache failure lost the committed instruction prefix");
	require(!uml::testing::cancel_pending_allocation_failure(),
		"post-execution cache allocation injection was not consumed");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active &&
		native_fpcr() == caller_fpcr,
		"post-execution cache failure leaked executor-private state");

	const auto retry = engine->run(request);
	require(retry.reason == uml::cpu::ExitReason::system_call &&
		retry.retired_instructions == 1 && cpu.pc == second_target + 4,
		"gadget engine was not reusable after post-execution failure");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active && native_fpcr() == caller_fpcr,
		"gadget retry did not retain balanced execution state");
}

void test_translated_internal_interrupt_obeys_fairness_budget()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 7,
		.instruction_budget = 64 * 1024,
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::instruction_budget,
		"internal translated event escaped as a guest interrupt");
	require(result.retired_instructions == 64 * 1024,
		"translated fairness retirement count changed");
	require(executor.block_calls == 16,
		"translated engine did not run through the exact fairness budget");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"translated execution lifetime was not balanced");
}

void test_translated_internal_interrupt_delivers_pending_interrupt()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	InterruptPollState poll{.pending_on_call = 2};
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 11,
		.instruction_budget = 64 * 1024,
		.interrupt = {
			.poll = poll_interrupt,
			.poll_context = &poll,
		},
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::interrupt,
		"pending host/guest interrupt was not delivered");
	require(result.retired_instructions == TranslatedBoundaryExecutor::block_size,
		"interrupt boundary lost the exact retired count");
	require(executor.block_calls == 1 && poll.calls == 2,
		"interrupt was not sampled at the translated cold boundary");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"interrupt exit did not close translated execution");
}

void test_translated_cleanup_is_exception_safe()
{
	ThrowingTranslatedExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 13,
		.instruction_budget = 1,
	};
	const auto failed =
		uml::cpu::arm64::detail::run_translated(executor, request);
	require(failed.reason == uml::cpu::ExitReason::fatal &&
		failed.retired_instructions == 0,
		"translated failure was not contained at the engine boundary");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"throwing translated entry did not perform exactly one cleanup");
	require(!executor.entered,
		"throwing translated entry left its execution context entered");

	executor.committed_before_throw = 3;
	const auto postamble_failed =
		uml::cpu::arm64::detail::run_translated(executor, request);
	require(postamble_failed.reason == uml::cpu::ExitReason::fatal &&
		postamble_failed.retired_instructions == 3,
		"translated postamble failure lost its committed prefix");
	require(executor.begin_calls == 2 && executor.end_calls == 2 &&
		!executor.entered,
		"translated postamble failure did not retain balanced cleanup");

	executor.throw_on_run = false;
	executor.committed_before_throw = 0;
	const auto retry = uml::cpu::arm64::detail::run_translated(executor, request);
	require(retry.reason == uml::cpu::ExitReason::fatal,
		"translated entry could not be retried after exception cleanup");
	require(executor.begin_calls == 3 && executor.end_calls == 3,
		"retried translated entry did not retain balanced cleanup");
}

/* The two translated entries answer one block event with the exit the
 * runtime expects of the ISA: the A64 entry reports floating-point traps and
 * SME accesses as themselves and a BKPT without a syndrome, and the AArch32
 * entry (an executor's run_a32_block) reports neither trap as anything but
 * fatal, carries BKPT's immediate, and names the PC, never the key. */
void test_translated_entries_report_the_exit_of_their_isa()
{
	using uml::cpu::EngineEvent;
	using uml::cpu::ExitReason;
	namespace key = uml::cpu::arm64::aarch32;
	constexpr std::uint64_t pc = 0x4000;
	struct Run final {
		uml::cpu::ExecutionResult result;
		ScriptedExecutor executor;
	};
	const auto run = [](bool aarch32, EngineEvent event) {
		Run run;
		run.executor.event = event;
		uml::memory::StaticGuestMemory memory;
		uml::cpu::arm64::CpuState cpu;
		cpu.pc = aarch32 ? key::a32_key(static_cast<std::uint32_t>(pc)) : pc;
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = 5,
			.instruction_budget = 64,
		};
		run.result = aarch32 ?
			uml::cpu::arm64::detail::run_translated_aarch32_blocks(
				run.executor, request) :
			uml::cpu::arm64::detail::run_translated(run.executor, request);
		return run;
	};

	for (const bool aarch32 : {false, true}) {
		const auto trap = run(aarch32, EngineEvent::floating_point_trap);
		require(trap.result.reason == (aarch32 ?
				ExitReason::fatal : ExitReason::floating_point_trap),
			"floating-point trap exit changed");
		require(trap.result.syndrome == (aarch32 ?
				0U : ScriptedExecutor::trap_syndrome),
			"floating-point trap syndrome changed");
		const auto sme = run(aarch32, EngineEvent::sme_access);
		require(sme.result.reason == (aarch32 ?
				ExitReason::fatal : ExitReason::sme_access),
			"SME access exit changed");
		const auto bkpt = run(aarch32, EngineEvent::breakpoint);
		require(bkpt.result.reason == ExitReason::breakpoint &&
			bkpt.result.syndrome == (aarch32 ?
				ScriptedExecutor::bkpt_immediate : 0U) &&
			bkpt.result.retired_instructions == 1,
			"breakpoint exit changed");
		const auto alignment = run(aarch32, EngineEvent::pc_alignment);
		require(alignment.result.reason == ExitReason::pc_alignment &&
			alignment.result.fault.address == pc &&
			alignment.result.fault.access ==
				uml::memory::MemoryAccess::execute,
			"PC-alignment event does not name the PC");
		const auto fault = run(aarch32, EngineEvent::memory_fault);
		require(fault.result.reason == ExitReason::memory_fault &&
			fault.result.fault.address ==
				ScriptedExecutor::fault_location &&
			fault.result.fault.access ==
				uml::memory::MemoryAccess::write &&
			fault.result.fault.alignment,
			"memory fault does not carry the executor's fault");
		for (const auto event : {EngineEvent::overflow,
			EngineEvent::general_protection, EngineEvent::trap_flag,
			EngineEvent::debug_trap, EngineEvent::bound_range,
			EngineEvent::fatal})
			require(run(aarch32, event).result.reason ==
				ExitReason::fatal, "non-arm64 event is not fatal");
		const auto system_call = run(aarch32, EngineEvent::system_call);
		require(system_call.result.reason == ExitReason::system_call &&
			system_call.executor.begin_pc == pc &&
			system_call.executor.a64_calls == (aarch32 ? 0U : 1U) &&
			system_call.executor.a32_calls == (aarch32 ? 1U : 0U) &&
			system_call.executor.end_calls == 1,
			"entry did not run its ISA's block from the PC");
		require(system_call.executor.a32_entries == (aarch32 ? 1U : 0U) &&
			system_call.executor.a32_entry_before_block == aarch32,
			"an AArch32 entry did not prepare its executor once, first");
	}
}

/* A PC that is not an instruction boundary of its ISA ends the entry before
 * any block runs: any A64 PC with a low bit set, and an A32 key with bit 1
 * set; a T32 key's halfword PC is valid and runs. */
void test_translated_entries_check_the_pc_at_each_block_boundary()
{
	using uml::cpu::EngineEvent;
	using uml::cpu::ExitReason;
	namespace key = uml::cpu::arm64::aarch32;
	const auto run = [](bool aarch32, std::uint64_t pc,
			    ScriptedExecutor &executor) {
		uml::memory::StaticGuestMemory memory;
		uml::cpu::arm64::CpuState cpu;
		cpu.pc = pc;
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = 5,
			.instruction_budget = 64,
		};
		return aarch32 ?
			uml::cpu::arm64::detail::run_translated_aarch32_blocks(
				executor, request) :
			uml::cpu::arm64::detail::run_translated(executor, request);
	};
	for (const std::uint64_t pc : {0x4001ULL, 0x4002ULL, 0x4003ULL}) {
		ScriptedExecutor executor;
		const auto result = run(false, pc, executor);
		require(result.reason == ExitReason::pc_alignment &&
			result.retired_instructions == 0 &&
			result.fault.address == pc &&
			result.fault.access == uml::memory::MemoryAccess::execute &&
			executor.a64_calls == 0 && executor.begin_pc == pc &&
			executor.begin_calls == 1 && executor.end_calls == 1,
			"misaligned A64 PC reached a block");
	}
	{
		ScriptedExecutor executor;
		const auto result = run(true, key::a32_key(0x4002), executor);
		require(result.reason == ExitReason::pc_alignment &&
			result.retired_instructions == 0 &&
			result.fault.address == 0x4002 &&
			result.fault.access == uml::memory::MemoryAccess::execute &&
			executor.a32_calls == 0 && executor.begin_pc == 0x4002 &&
			executor.end_calls == 1,
			"A32 key with bit 1 set reached a block");
	}
	{
		ScriptedExecutor executor;
		executor.event = EngineEvent::system_call;
		const auto result = run(true, key::t32_key(0x4002), executor);
		require(result.reason == ExitReason::system_call &&
			executor.a32_calls == 1 && executor.begin_pc == 0x4002,
			"T32 halfword PC did not run");
	}
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	try {
		test_factory_contract();
		test_all_engines_scope_unsupported_instruction_to_guest();
		test_all_engines_preserve_fault_direction_and_retry();
		test_production_translated_exception_contract();
		test_gadget_post_execution_failure_preserves_prefix_and_reuse();
		/* C-bridge engines are retained by the routed test Context. Exercise
		 * standalone JIT admission above before that test-lifetime cache
		 * intentionally occupies the configured executor slot. */
		test_c_bridge_preserves_fault_direction_and_retry();
		test_translated_internal_interrupt_obeys_fairness_budget();
		test_translated_internal_interrupt_delivers_pending_interrupt();
		test_translated_cleanup_is_exception_safe();
		test_translated_entries_report_the_exit_of_their_isa();
		test_translated_entries_check_the_pc_at_each_block_boundary();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "CPU engine contract test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
