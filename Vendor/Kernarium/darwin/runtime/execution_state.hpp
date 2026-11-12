// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine.hpp"
#if DARWIN_UML_X86_64_GUEST
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "runtime/x86_gadget_runtime.hpp"
#endif
#if DARWIN_UML_X86_64_JIT
#include "runtime/x86_jit_runtime.hpp"
#include "runtime/x86_tier.hpp"
#endif
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"

#include <cstddef>
#include <memory>

namespace uml::runtime {

struct GuestInterruptSequence final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
};

class ThreadEngineSlots final {
public:
	template<::uml::cpu::EngineKind Kind>
	[[nodiscard]] std::unique_ptr<::uml::cpu::CpuEngine> &slot() noexcept
	{
		static_assert(Kind == ::uml::cpu::EngineKind::gadgets ||
			Kind == ::uml::cpu::EngineKind::jit,
			"unsupported CPU engine kind");
		return Kind == ::uml::cpu::EngineKind::gadgets ? gadgets_ : jit_;
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return gadgets_ == nullptr && jit_ == nullptr;
	}

	[[nodiscard]] std::size_t active_count() const noexcept
	{
		return static_cast<std::size_t>(gadgets_ != nullptr) +
			static_cast<std::size_t>(jit_ != nullptr);
	}

private:
	std::unique_ptr<::uml::cpu::CpuEngine> gadgets_;
	std::unique_ptr<::uml::cpu::CpuEngine> jit_;
};

struct ThreadExecutionState final {
	/* The register file is sized once, for the host's streaming vector
	 * length, when the run implements SME; otherwise it is empty and costs
	 * nothing. */
	ThreadExecutionState()
	{
		cpu.bind_sme_registers(sme_registers);
	}

	::uml::cpu::arm64::CpuState cpu{};
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	::uml::cpu::arm64::SmeRegisterFile sme_registers;
#else
	::uml::cpu::arm64::SmeRegisterFile sme_registers{
		::uml::cpu::arm64::SmeRegisterFile::for_host(
			::uml::platform::apple::probed_host_capabilities())};
#endif
#if DARWIN_UML_X86_64_GUEST
	::uml::cpu::x86_64::CpuState x86_cpu{};
	::uml::cpu::x86_64::gadgets::Executor x86_executor{};
	/* One for each set of exceptions the runtime checks
	 * (`checked_exceptions`), made the first time the guest needs it. */
	X86CheckedExecutors x86_checked_executors;
#endif
	::uml::memory::StaticGuestMemory memory;
	ThreadEngineSlots engines;
	GuestInterruptSequence interrupt;
#if DARWIN_UML_X86_64_JIT
	/* Last, so the members the gadget path reads keep their offsets. */
	X86JitEngine x86_jit;
#endif
#if DARWIN_UML_X86_64_GUEST
	/* Whether the current x86 dispatch's mm is shared with another task
	 * (DARWIN_UML_DISPATCH_MM_SHARED), for the x86 JIT's memory ordering.
	 * After every other member, so none of their offsets moves. */
	bool x86_mm_shared{};
#endif
#if DARWIN_UML_X86_64_JIT
	/* The tier state of the address space this vCPU last ran in. */
	std::shared_ptr<X86TierState> x86_tier;
	unsigned long x86_tier_generation{};
#endif
};

} // namespace uml::runtime
