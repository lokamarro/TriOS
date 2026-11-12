// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"
#include "memory/tlb.hpp"

namespace uml::cpu::arm64 {

void activate_native_fp_environment(CpuState &cpu) noexcept;

using ::uml::memory::Tlb;

extern "C" std::uint64_t
uml_arm64_jit_alignment_fault(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate(Tlb *, std::uint64_t, unsigned) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate_preserving(Tlb *, std::uint64_t,
                                       unsigned) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_exclusive(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_memory(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_pair(CpuState *, Tlb *, std::uint32_t) noexcept;
/* FEAT_MOPS: one stage, or a whole prologue/main/epilogue sequence when
 * stages is 3. */
extern "C" std::uint64_t
uml_arm64_jit_execute_mops(CpuState *, Tlb *, std::uint32_t,
                           std::uint32_t stages) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_ordered(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_simd_structure(CpuState *, Tlb *,
                                     std::uint32_t) noexcept;
/* SME (jit_semantic_sme.cpp): a guest write of SVCR (MSR SVCR, SMSTART,
 * SMSTOP), and the slow path of the control core's ZA and ZT0 forms. Both
 * run on the register file, which the leave sequence has filled. */
extern "C" std::uint64_t
uml_arm64_jit_write_svcr(CpuState *, Tlb *, std::uint32_t) noexcept;
extern "C" std::uint64_t
uml_arm64_jit_execute_sme(CpuState *, Tlb *, std::uint32_t) noexcept;
/* The slow path of the SM = 0 vector-length family's spill words
 * (detail/sm0_vector_length.hpp). */
extern "C" std::uint64_t
uml_arm64_jit_execute_sm0_spill(CpuState *, Tlb *, std::uint32_t) noexcept;

} // namespace uml::cpu::arm64
