// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/execution_types.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::layout {

inline constexpr unsigned version = UML_ARM64_ASSEMBLY_LAYOUT_VERSION;

static_assert(offsetof(CpuState, registers) == UML_GADGET_CPU_X0);
static_assert(offsetof(CpuState, sp) == UML_GADGET_CPU_SP);
static_assert(offsetof(CpuState, pc) == UML_GADGET_CPU_PC);
static_assert(offsetof(CpuState, nzcv) == UML_GADGET_CPU_NZCV);
static_assert(offsetof(CpuState, vectors) == UML_GADGET_CPU_V0);
static_assert(offsetof(CpuState, fpcr) == UML_GADGET_CPU_FPCR);
static_assert(offsetof(CpuState, fpsr) == UML_GADGET_CPU_FPSR);
static_assert(offsetof(CpuState, tls) == UML_GADGET_CPU_TLS);
static_assert(offsetof(CpuState, tbi_enabled) == UML_GADGET_CPU_TBI_ENABLED);
static_assert(offsetof(CpuState, pstate_dit) == UML_GADGET_CPU_PSTATE_DIT);
static_assert(offsetof(CpuState, pstate_daif_access) == UML_GADGET_CPU_PSTATE_DAIF_ACCESS);
static_assert(offsetof(CpuState, pstate_daif) == UML_GADGET_CPU_PSTATE_DAIF);
static_assert(offsetof(CpuState, exclusive_address) == UML_GADGET_CPU_EXCLUSIVE_ADDRESS);
static_assert(offsetof(CpuState, exclusive_key) == UML_GADGET_CPU_EXCLUSIVE_KEY);
static_assert(offsetof(CpuState, exclusive_value) == UML_GADGET_CPU_EXCLUSIVE_VALUE);
static_assert(offsetof(CpuState, exclusive_value2) == UML_GADGET_CPU_EXCLUSIVE_VALUE2);
static_assert(offsetof(CpuState, exclusive_version) == UML_GADGET_CPU_EXCLUSIVE_VERSION);
/* engine/exclusive.inc records a load's host address, value and second value
 * with one paired store and one store. */
static_assert(UML_GADGET_CPU_EXCLUSIVE_VALUE == UML_GADGET_CPU_EXCLUSIVE_KEY + 8);
static_assert(UML_GADGET_CPU_EXCLUSIVE_VALUE2 == UML_GADGET_CPU_EXCLUSIVE_KEY + 16);
static_assert(offsetof(CpuState, exclusive_size) == UML_GADGET_CPU_EXCLUSIVE_SIZE);
static_assert(UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT == exclusive_key_reentered_bit);
static_assert(offsetof(CpuState, deferred_vectors) == UML_GADGET_CPU_DEFERRED_VECTORS);
static_assert(offsetof(CpuState, deferred_fpcr) == UML_GADGET_CPU_DEFERRED_FPCR);
static_assert(offsetof(CpuState, deferred_fpsr) == UML_GADGET_CPU_DEFERRED_FPSR);
static_assert(offsetof(CpuState, fp_loaded) == UML_GADGET_CPU_FP_LOADED);
static_assert(offsetof(CpuState, fp_dirty) == UML_GADGET_CPU_FP_DIRTY);
static_assert(offsetof(CpuState, gadget_zero) == UML_GADGET_CPU_ZERO);
static_assert(offsetof(CpuState, gadget_host_fpcr) == UML_ARM64_CPU_HOST_FPCR);
static_assert(offsetof(CpuState, gadget_host_fpsr) == UML_ARM64_CPU_HOST_FPSR);
static_assert(offsetof(CpuState, gadget_fp_active) == UML_ARM64_CPU_FP_ACTIVE);
static_assert(offsetof(CpuState, gadget_indirect_miss) == UML_GADGET_CPU_INDIRECT_MISS);
static_assert(offsetof(CpuState, gadget_return_stack) == UML_GADGET_CPU_RETURN_STACK);
static_assert(offsetof(CpuState, gadget_return_top) == UML_GADGET_CPU_RETURN_TOP);
static_assert(CpuState::return_stack_entries == UML_GADGET_RETURN_STACK_ENTRIES);
static_assert(offsetof(CpuState, gadget_target_tier) == UML_GADGET_CPU_TARGET_TIER);
static_assert(sizeof(CpuState::ReturnRecord) == 16);
static_assert(offsetof(CpuState, tpidr2) == UML_ARM64_CPU_TPIDR2);
static_assert(offsetof(CpuState, svcr) == UML_ARM64_CPU_SVCR);
static_assert(offsetof(CpuState, sme_loaded) == UML_ARM64_CPU_SME_LOADED);
static_assert(offsetof(CpuState, sme_dirty) == UML_ARM64_CPU_SME_DIRTY);
static_assert(offsetof(CpuState, sme_host_mode) == UML_ARM64_CPU_SME_HOST_MODE);
static_assert(offsetof(CpuState, sme_svl) == UML_ARM64_CPU_SME_SVL);
static_assert(offsetof(CpuState, sme_task) == UML_ARM64_CPU_SME_TASK);
static_assert(offsetof(CpuState, sme_registers) == UML_ARM64_CPU_SME_REGISTERS);
static_assert(UML_ARM64_SVCR_SM == svcr_sm && UML_ARM64_SVCR_ZA == svcr_za);
static_assert(UML_ARM64_SME_HOST_STREAMING == sme_host_streaming);
static_assert(UML_ARM64_SME_HOST_ZA == sme_host_za);
static_assert(UML_ARM64_SME_HOST_STREAMING_RESIDENT ==
	sme_host_streaming_resident);
static_assert(UML_ARM64_SME_HOST_ZA_RESIDENT == sme_host_za_resident);
static_assert(UML_ARM64_SME_ZT0_BYTES == sme_zt0_bytes);
/* The assembly-visible layout is the C++ one at every SVL. */
[[nodiscard]] constexpr bool sme_layout_matches(std::size_t svl) noexcept
{
	return sme_z_offset(svl, 31) == 31 * svl &&
		sme_p_offset(svl, 15) ==
			UML_ARM64_SME_P_OFFSET_VL * svl + 15 * (svl / 8) &&
		sme_za_offset(svl, 0) == UML_ARM64_SME_ZA_OFFSET_VL * svl &&
		sme_zt0_offset(svl) ==
			(UML_ARM64_SME_ZA_OFFSET_VL + svl) * svl &&
		sme_storage_size(svl) ==
			sme_zt0_offset(svl) + UML_ARM64_SME_ZT0_BYTES;
}
static_assert(sme_layout_matches(16) && sme_layout_matches(32) &&
	sme_layout_matches(64) && sme_layout_matches(128) &&
	sme_layout_matches(256));
static_assert(offsetof(CpuState, a32_ge_mask) == UML_ARM64_CPU_A32_GE_MASK);
static_assert(offsetof(CpuState, a32_fpscr_nzcv) ==
	UML_ARM64_CPU_A32_FPSCR_NZCV);
static_assert(offsetof(CpuState, a32_tpidruro) == UML_ARM64_CPU_A32_TPIDRURO);
static_assert(offsetof(CpuState, a32_q) == UML_ARM64_CPU_A32_Q);
static_assert(offsetof(CpuState, a32_itstate) == UML_ARM64_CPU_A32_ITSTATE);
static_assert(offsetof(CpuState, a32_cp15ben) == UML_ARM64_CPU_A32_CP15BEN);
static_assert(offsetof(CpuState, a32) == UML_ARM64_CPU_A32);
static_assert(offsetof(CpuState, a32_fp_standard) ==
	      UML_ARM64_CPU_A32_FP_STANDARD);
/* The AArch32 block follows the reserved block and every earlier offset
 * stands, so no existing gadget or emitted instruction changes; its last
 * byte fits the 16-bit operand a gadget carries. */
static_assert(UML_ARM64_CPU_A32_FP_STANDARD < 0x1'0000);
static_assert(offsetof(CpuState, code_sequence) == UML_ARM64_CPU_CODE_SEQUENCE);
static_assert(offsetof(CpuState, code_sequence_seen) ==
	UML_ARM64_CPU_CODE_SEQUENCE_SEEN);
/* Each is one scaled LDR from CpuState. */
static_assert(UML_ARM64_CPU_CODE_SEQUENCE > UML_ARM64_CPU_A32_FP_STANDARD &&
	UML_ARM64_CPU_CODE_SEQUENCE % 8 == 0 &&
	UML_ARM64_CPU_CODE_SEQUENCE_SEEN % 8 == 0 &&
	UML_ARM64_CPU_CODE_SEQUENCE_SEEN < 4096 * 8);

static_assert(sizeof(::uml::memory::TlbEntry) == 32);
static_assert(alignof(::uml::memory::TlbEntry) == 32);
static_assert(offsetof(::uml::memory::Tlb, entries) == UML_GADGET_TLB_ENTRIES);
static_assert(offsetof(::uml::memory::TlbEntry, read_page) == UML_GADGET_TLB_ENTRY_READ_PAGE);
static_assert(offsetof(::uml::memory::TlbEntry, read_data_minus_address) == UML_GADGET_TLB_ENTRY_READ_DATA_MINUS_ADDRESS);
static_assert(offsetof(::uml::memory::TlbEntry, write_page) == UML_GADGET_TLB_ENTRY_WRITE_PAGE);
static_assert(offsetof(::uml::memory::TlbEntry, write_data_minus_address) == UML_GADGET_TLB_ENTRY_WRITE_DATA_MINUS_ADDRESS);
static_assert(offsetof(::uml::memory::Tlb, fault_address) == UML_GADGET_TLB_FAULT_ADDRESS);
static_assert(offsetof(::uml::memory::Tlb, fault_access) == UML_GADGET_TLB_FAULT_ACCESS);
static_assert(offsetof(::uml::memory::Tlb, unaligned_atomics) == UML_GADGET_TLB_UNALIGNED_ATOMICS);

/* Gadgets return these in w0 and the executor casts them to EngineEvent. */
static_assert(UML_GADGET_EVENT_SYSCALL == static_cast<int>(EngineEvent::system_call));
static_assert(UML_GADGET_EVENT_MEMORY_FAULT == static_cast<int>(EngineEvent::memory_fault));
static_assert(UML_GADGET_EVENT_INTERRUPT == static_cast<int>(EngineEvent::interrupt));
static_assert(UML_GADGET_EVENT_ILLEGAL == static_cast<int>(EngineEvent::illegal_instruction));
static_assert(UML_GADGET_EVENT_BREAKPOINT == static_cast<int>(EngineEvent::breakpoint));
static_assert(UML_GADGET_EVENT_SME_ACCESS == static_cast<int>(EngineEvent::sme_access));

static_assert(UML_ARM64_FPCR_WRITABLE == fpcr_writable_mask);
static_assert(UML_ARM64_FPCR_AFP_CONTROLS == fpcr_afp_controls);
static_assert(UML_ARM64_FPCR_TRAP_ENABLES == detail::fpcr_exception_enable_mask);
static_assert(UML_ARM64_FPSR_WRITABLE == fpsr_writable_mask);
static_assert((fpcr_writable_mask & detail::fpcr_exception_enable_mask) ==
	detail::fpcr_exception_enable_mask);

} // namespace uml::cpu::arm64::layout
