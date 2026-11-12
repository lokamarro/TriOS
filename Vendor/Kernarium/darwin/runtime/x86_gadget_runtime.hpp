// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if DARWIN_UML_X86_64_GUEST

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "memory/static_guest_memory.hpp"
#include "runtime/x86_runtime_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::runtime {

/* `instruction_budget` bounds how long one dispatch runs, and the block that
 * crosses it runs to its end, as a block of the arm64 engines does. With
 * `exact_budget` the run instead stops at exactly that many instructions,
 * which is what a single step needs.
 *
 * While `checked_exceptions` finds exceptions whose instructions the runtime
 * checks -- #AC, #XM, or #MF pending for the next waiting x87 instruction --
 * the run uses the executor of `checked_executors` for that set, made on
 * first use, whose blocks stop before every instruction that could raise one;
 * it runs those singly, checked, and the rest linked, as `executor` runs
 * everything otherwise. */
using X86CheckedExecutors = std::array<
	std::unique_ptr<::uml::cpu::x86_64::gadgets::Executor>, 8>;

[[nodiscard]] X86RunResult run_x86_gadgets(
	::uml::cpu::x86_64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory,
	::uml::cpu::x86_64::gadgets::Executor &executor,
	X86CheckedExecutors &checked_executors,
	unsigned long generation, std::size_t instruction_budget,
	bool exact_budget, const ::uml::cpu::InterruptToken &interrupt) noexcept;

} // namespace uml::runtime

#endif
