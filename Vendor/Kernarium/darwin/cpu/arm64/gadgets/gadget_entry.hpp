// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "tlb.hpp"
#include "execution_histogram.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::gadgets {

struct EngineResult final {
	std::int32_t event;
	std::uint32_t reserved;
	std::size_t executed;
};

extern "C" EngineResult uml_arm64_gadget_enter(
	const std::uintptr_t *program, CpuState *cpu, ::uml::memory::Tlb *tlb,
	std::size_t initial_instructions
#if UML_GADGET_EXECUTION_HISTOGRAM
	, void *execution_counters
#endif
	);

/* uml_arm64_gadget_enter for a program translated for SVCR.SM = 1, in the
 * SME part: it puts the host in the guest's SME mode first, with guest ZA
 * and ZT0 loaded into host ZA and ZT0, and takes it out again after. */
extern "C" EngineResult uml_arm64_gadget_enter_streaming(
	const std::uintptr_t *program, CpuState *cpu, ::uml::memory::Tlb *tlb,
	std::size_t initial_instructions
#if UML_GADGET_EXECUTION_HISTOGRAM
	, void *execution_counters
#endif
	);

} // namespace uml::cpu::arm64::gadgets
