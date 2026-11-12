// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What every x86-64 engine's runtime shares: the result of a dispatch, the
 * static MMU execution scope it runs in, how a return to the signal
 * trampoline ends it, the mapping of a non-canonical access to the exception
 * x86 raises for it, and whether a generation is live. */

#if DARWIN_UML_X86_64_GUEST

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/segments.hpp"
#include "memory/static_guest_memory.hpp"
#include "uml_darwin_static_mmu.h"

#include <cstdint>

namespace uml::runtime {

/* x86 exception vectors the runtime reports as a breakpoint's syndrome. */
inline constexpr std::uint32_t x86_stack_segment_fault = 12;
inline constexpr std::uint32_t x86_general_protection = 13;

/* What one dispatch of x86 code left. */
struct X86RunResult final {
	::uml::cpu::ExecutionResult execution;
	/* The error code of a #GP or #SS the run ended in: for INT n, the
	 * vector's IDT entry. For #MF, the x87 status word with the control
	 * word above it, and for #XM, MXCSR: what Linux derives the signal's
	 * code from. */
	std::int32_t error_code{};
};

class X86StaticExecutionScope final {
public:
	X86StaticExecutionScope(unsigned long generation, std::uint64_t guest_pc)
		: context_{darwin_uml_static_vm_execution_context_create(generation)}
	{
		darwin_uml_static_vm_require_coherent_kernel_code_writes();
		entered_ = context_ != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0;
	}

	~X86StaticExecutionScope()
	{
		if (entered_)
			(void)darwin_uml_static_vm_execution_context_end(context_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	X86StaticExecutionScope(const X86StaticExecutionScope &) = delete;
	X86StaticExecutionScope &operator=(const X86StaticExecutionScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }
	[[nodiscard]] void *context() const noexcept { return context_; }
	[[nodiscard]] darwin_uml_static_sequences sequences() const noexcept
	{
		return darwin_uml_static_vm_execution_context_sequences(context_);
	}

private:
	void *context_{};
	bool entered_{};
};

/* Bits 63 to 47 all equal: an address of the 4-level paging x86-64 Linux
 * runs userspace under. */
[[nodiscard]] constexpr bool x86_canonical(std::uint64_t address) noexcept
{
	const auto high = static_cast<std::int64_t>(address) >> 47;
	return high == 0 || high == -1;
}

/* A non-canonical address raises #SS when the stack segment forms it --
 * an implicit stack access, or an operand based on RSP or RBP with no
 * segment override -- and #GP otherwise, neither with an error code. */
[[nodiscard]] std::uint32_t x86_noncanonical_vector(
	::uml::memory::StaticGuestMemory &memory,
	const ::uml::cpu::x86_64::CpuState &cpu) noexcept;

/* A return to the signal trampoline is rt_sigreturn, which the run reports
 * as the system call. */
inline void x86_signal_return(::uml::cpu::x86_64::CpuState &cpu,
	::uml::cpu::ExecutionResult &result) noexcept
{
	cpu.gpr(::uml::cpu::x86_64::Register::rax) = 15;
	cpu.system_call_32bit = false;
	result.reason = ::uml::cpu::ExitReason::system_call;
}

/* Whether the host still has the generation: it ends with its process. A host
 * that cannot say counts as live. Static, so that each unit that passes its
 * address on calls its own copy directly. */
[[nodiscard]] static inline bool x86_generation_live(
	unsigned long generation) noexcept
{
	return darwin_uml_static_vm_generation_live(generation) != 0;
}

/* With UML_X86_MM_SHARED_LOG set to a writable path, called for each x86
 * dispatch with the kernel's mark of whether its mm is shared
 * (DARWIN_UML_DISPATCH_MM_SHARED). The runtime appends an "mm-shared" line
 * whenever an address space's (generation's) mark differs from its last
 * dispatch's, starting from unshared, with the mark and the RIP the dispatch
 * resumed at; tests/integration/x86_64/run_guest_x86_64_mm_shared.py reads
 * it. It is a file rather than stderr because the emulator's stderr shares
 * the guest console. The variable is read once, at the process's first x86
 * dispatch; nothing is opened, locked or recorded when it is unset. */
void note_x86_mm_shared(unsigned long generation, bool shared,
	std::uint64_t rip) noexcept;

/* Whether UML_X86_MM_SHARED_LOG names a log the runtime could open. */
[[nodiscard]] bool x86_mm_shared_logging() noexcept;

/* With the log open, called after each x86-64 JIT dispatch with the
 * kernel's mark and whether its code kept x86's store order. The runtime
 * appends a "jit-space" line for each address space's first dispatch and
 * whenever its ordering changes, with the RIP the dispatch started at: the
 * test's proof of which code a space ran. A dispatch the kernel marked
 * shared that ran unordered code appends a "jit-unordered-shared" line,
 * which must never appear. */
void note_x86_jit_ordering(unsigned long generation, bool marked,
	bool ordered, std::uint64_t rip) noexcept;

} // namespace uml::runtime

#endif
