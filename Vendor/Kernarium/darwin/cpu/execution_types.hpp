// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace uml::cpu {

/* ISA-neutral exits shared by every guest CPU backend. */
enum class EngineEvent : int {
	fatal = -1,
	continued = 0,
	system_call = 1,
	memory_fault = 2,
	interrupt = 3,
	illegal_instruction = 4,
	breakpoint = 5,
	floating_point_trap = 7,
	pc_alignment = 8,
	overflow = 9,
	general_protection = 10,
	/* x86 POPF or IRET set RFLAGS.TF: the next instruction raises #DB after it
	 * retires. RIP names that instruction. */
	trap_flag = 11,
	/* x86 ICEBP raised #DB as a trap. RIP names the next instruction. */
	debug_trap = 12,
	/* x86 BOUND found its index outside the bounds: #BR, a fault. RIP names
	 * the BOUND. */
	bound_range = 13,
	/* arm64: an instruction would set PSTATE.SM or PSTATE.ZA for a task
	 * with no SME storage. It did not retire; the kernel allocates the
	 * storage and enters the task again at the same PC. */
	sme_access = 14,
};

/* An exit of one of these kinds reports the instruction that raised it rather
 * than its successor, so that instruction did not retire. This is the arm64
 * engines' classification; it does not cover the x86-only exits. */
[[nodiscard]] constexpr bool terminal_does_not_retire(EngineEvent event) noexcept
{
	switch (event) {
	case EngineEvent::fatal:
	case EngineEvent::memory_fault:
	case EngineEvent::illegal_instruction:
	case EngineEvent::floating_point_trap:
	case EngineEvent::sme_access:
		return true;
	default:
		return false;
	}
}

enum class ExitReason {
	system_call,
	memory_fault,
	interrupt,
	illegal_instruction,
	breakpoint,
	floating_point_trap,
	pc_alignment,
	instruction_budget,
	/* A requested single step retired one instruction and had no other exit. */
	single_step,
	fatal,
	/* EngineEvent::sme_access. */
	sme_access,
};

using InterruptPoll = bool (*)(void *) noexcept;

struct FaultInfo final {
	std::uint64_t address{};
	::uml::memory::MemoryAccess access{::uml::memory::MemoryAccess::read};
	bool alignment{};
};

struct InterruptToken final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
	const std::atomic_bool *stop{};
	InterruptPoll poll{};
	void *poll_context{};

	[[nodiscard]] bool state_pending() const noexcept
	{
		return (sequence != nullptr &&
			__atomic_load_n(sequence, __ATOMIC_ACQUIRE) != snapshot) ||
			(stop != nullptr && stop->load(std::memory_order_acquire));
	}

	[[nodiscard]] bool callback_pending() const noexcept
	{
		return poll != nullptr && poll(poll_context);
	}

	[[nodiscard]] bool pending() const noexcept
	{
		return state_pending() || callback_pending();
	}
};

struct ExecutionResult final {
	ExitReason reason{ExitReason::fatal};
	std::size_t retired_instructions{};
	FaultInfo fault{};
	std::uint32_t syndrome{};
};

} // namespace uml::cpu
