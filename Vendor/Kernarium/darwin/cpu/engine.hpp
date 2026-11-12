/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/execution_types.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::memory {
class StaticGuestMemory;
}

namespace uml::cpu {

using GuestAddressSpace = ::uml::memory::GuestAddressSpace;

enum class EngineKind {
	gadgets,
	jit,
};

using BoundaryHook = bool (*)(arm64::CpuState &, void *) noexcept;

struct ExecutionRequest final {
	arm64::CpuState &cpu;
	GuestAddressSpace &memory;
	/* The runtime supplies this typed view whenever memory is backed by the
	 * static MMU. Translated engines require it for their fast path. Keeping
	 * the view explicit removes RTTI from every guest entry. */
	::uml::memory::StaticGuestMemory *translated_memory{};
	unsigned long generation{};
	std::size_t instruction_budget{};
	InterruptToken interrupt{};
	BoundaryHook boundary{};
	void *boundary_context{};
	/* Retire at most one instruction and report single_step when it
	 * completes without another exit, as PSTATE.SS does. */
	bool single_step{};
};

struct EngineConfiguration final {
	HostCapabilities host_capabilities{::uml::cpu::host_capabilities()};
};

class CpuEngine {
public:
	virtual ~CpuEngine() = default;
	[[nodiscard]] virtual EngineKind kind() const noexcept = 0;
	/* Guest exits, including an internal host-resource failure, are returned as
	 * ExecutionResult. This boundary is noexcept because it is called from the
	 * fixed-arity C kernel bridge; concrete engines must not unwind through it.
	 * Before returning, an engine must also commit any guest state held in host
	 * architectural registers and release its engine-private execution state.
	 * Callers must never need an engine-specific cleanup operation after run().
	 */
	[[nodiscard]] virtual ExecutionResult run(ExecutionRequest &) noexcept = 0;
	/* Runs an AArch32 EL0 task: request.cpu holds its state as
	 * arm64/aarch32/guest_key.hpp and CpuState describe, with the PC in
	 * key form. The runtime enters here instead of run() for a task whose
	 * PSTATE.nRW says AArch32, so an A64 entry pays nothing for it. An
	 * engine without AArch32, which today is only a test engine, takes it
	 * as the illegal-instruction exit without touching the state. */
	[[nodiscard]] virtual ExecutionResult run_aarch32(
		ExecutionRequest &) noexcept
	{
		return {.reason = ExitReason::illegal_instruction};
	}
};

} // namespace uml::cpu
