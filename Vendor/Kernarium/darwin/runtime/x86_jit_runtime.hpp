// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The x86-64 product's seam to the macOS-only x86-64 JIT. Only the unit
 * behind this header includes the engine; the rest of the runtime holds it
 * through an opaque owner. The macOS x86-64 product alone compiles it
 * (DARWIN_UML_X86_64_JIT). */

#if DARWIN_UML_X86_64_JIT

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"
#include "runtime/x86_runtime_support.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::cpu::x86_64::jit {
class FexEngine;
} // namespace uml::cpu::x86_64::jit

namespace uml::runtime {

struct X86JitEngineDeleter final {
	void operator()(::uml::cpu::x86_64::jit::FexEngine *engine) const noexcept;
};

/* One vCPU thread's engine, made on the first dispatch that selects it. */
using X86JitEngine = std::unique_ptr<::uml::cpu::x86_64::jit::FexEngine,
	X86JitEngineDeleter>;

/* Whether this host can run the engine. Decided once. */
[[nodiscard]] bool x86_jit_available() noexcept;

/* One dispatch of x86 code. The result is the gadget runtime's, so the
 * caller reports it to Linux the same way. `mm_shared` is the
 * kernel's mark that another agent may observe the address space's memory
 * (DARWIN_UML_DISPATCH_MM_SHARED): the dispatch then keeps x86's total
 * store order. */
[[nodiscard]] X86RunResult run_x86_jit(
	::uml::cpu::x86_64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory, X86JitEngine &engine,
	unsigned long generation, std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt, bool mm_shared) noexcept;

} // namespace uml::runtime

#endif
