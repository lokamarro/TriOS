// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/layout/assembly_offsets.h"

#include <cstddef>

namespace uml::cpu::x86_64::layout {

static_assert(offsetof(CpuState, registers) == UML_X86_64_CPU_REGISTERS);
static_assert(offsetof(CpuState, vectors) == UML_X86_64_CPU_VECTORS);
static_assert(offsetof(CpuState, mmx) == UML_X86_64_CPU_MMX);
static_assert(offsetof(CpuState, x87) == UML_X86_64_CPU_X87);
static_assert(offsetof(CpuState, segment_selectors) ==
	UML_X86_64_CPU_SEGMENT_SELECTORS);
static_assert(offsetof(CpuState, segment_bypass) ==
	UML_X86_64_CPU_SEGMENT_BYPASS);
static_assert(offsetof(CpuState, rip) == UML_X86_64_CPU_RIP);
static_assert(offsetof(CpuState, rflags) == UML_X86_64_CPU_RFLAGS);
static_assert(
	offsetof(CpuState, segment_zero) ==
	UML_X86_64_CPU_SEGMENT_ZERO);
static_assert(offsetof(CpuState, fs_base) - offsetof(CpuState, segment_zero) == 8);
static_assert(offsetof(CpuState, gs_base) - offsetof(CpuState, fs_base) == 8);
static_assert(offsetof(CpuState, fs_base) == UML_X86_64_CPU_FS_BASE);
static_assert(offsetof(CpuState, gs_base) == UML_X86_64_CPU_GS_BASE);
static_assert(offsetof(CpuState, mxcsr) == UML_X86_64_CPU_MXCSR);
static_assert(offsetof(CpuState, x87_control) == UML_X86_64_CPU_X87_CONTROL);
static_assert(offsetof(CpuState, x87_status) == UML_X86_64_CPU_X87_STATUS);
static_assert(offsetof(CpuState, x87_tag) == UML_X86_64_CPU_X87_TAG);
static_assert(offsetof(CpuState, x87_top) == UML_X86_64_CPU_X87_TOP);
static_assert(offsetof(CpuState, exception_checks) ==
	UML_X86_64_CPU_EXCEPTION_CHECKS);
static_assert(offsetof(CpuState, system_call_32bit) ==
	UML_X86_64_CPU_SYSTEM_CALL_32BIT);
static_assert(offsetof(CpuState, execution_mode) ==
	UML_X86_64_CPU_EXECUTION_MODE);
static_assert(offsetof(CpuState, timestamp_faults) ==
	UML_X86_64_CPU_TIMESTAMP_FAULTS);
static_assert(offsetof(CpuState, exception_raised) ==
	UML_X86_64_CPU_EXCEPTION_RAISED);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, left) == UML_X86_64_CPU_DEFERRED_LEFT);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, right) == UML_X86_64_CPU_DEFERRED_RIGHT);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, result) == UML_X86_64_CPU_DEFERRED_RESULT);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, operation) == UML_X86_64_CPU_DEFERRED_OPERATION);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, width) == UML_X86_64_CPU_DEFERRED_WIDTH);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, carry) == UML_X86_64_CPU_DEFERRED_CARRY);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, carry_pending) ==
	UML_X86_64_CPU_DEFERRED_PENDING);
static_assert(offsetof(CpuState, carry_slot) == UML_X86_64_CPU_CARRY_SLOT);
static_assert(offsetof(CpuState, segments) == UML_X86_64_CPU_SEGMENTS);
static_assert(sizeof(SegmentCache) == UML_X86_64_SEGMENT_CACHE_SIZE);
static_assert(offsetof(SegmentCache, check_low) ==
	UML_X86_64_SEGMENT_CHECK_LOW);
static_assert(offsetof(SegmentCache, check_span) ==
	UML_X86_64_SEGMENT_CHECK_SPAN);
static_assert(offsetof(SegmentCache, base) == UML_X86_64_SEGMENT_BASE);
static_assert(sizeof(DeferredFlags) == 32);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, override_mask) ==
	UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK);
static_assert(offsetof(CpuState, deferred_flags) +
	offsetof(DeferredFlags, override_value) ==
	UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE);

/* Gadgets return these in w0 and the executor casts them to EngineEvent. */
static_assert(UML_X86_64_EVENT_CONTINUED ==
	static_cast<int>(EngineEvent::continued));
static_assert(UML_X86_64_EVENT_SYSCALL ==
	static_cast<int>(EngineEvent::system_call));
static_assert(UML_X86_64_EVENT_MEMORY_FAULT ==
	static_cast<int>(EngineEvent::memory_fault));
static_assert(UML_X86_64_EVENT_BREAKPOINT ==
	static_cast<int>(EngineEvent::breakpoint));
static_assert(UML_X86_64_EVENT_FLOATING_POINT_TRAP ==
	static_cast<int>(EngineEvent::floating_point_trap));
static_assert(UML_X86_64_EVENT_OVERFLOW ==
	static_cast<int>(EngineEvent::overflow));
static_assert(UML_X86_64_EVENT_GENERAL_PROTECTION ==
	static_cast<int>(EngineEvent::general_protection));
static_assert(UML_X86_64_EVENT_TRAP_FLAG ==
	static_cast<int>(EngineEvent::trap_flag));
static_assert(UML_X86_64_EVENT_DEBUG_TRAP ==
	static_cast<int>(EngineEvent::debug_trap));
static_assert(UML_X86_64_EVENT_BOUND_RANGE ==
	static_cast<int>(EngineEvent::bound_range));

} // namespace uml::cpu::x86_64::layout
