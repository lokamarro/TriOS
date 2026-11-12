// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/tlb.hpp"

#include <cstdint>

namespace uml::cpu::arm64 {

/* The terminal slow path of the AArch32 translation: it executes the one
 * instruction `word` (an A32 word, or hw1 << 16 | hw2 of a T32 instruction)
 * at the PC key `key`, under ITSTATE `itstate`, on the register file that the
 * generated code spilled, and returns a packed JIT result for it. The
 * instruction is either retired, with CpuState's PC key and ITSTATE
 * advanced past it or to its branch target, or it faults and changes
 * nothing. The instruction runs in the AArch32 semantic library, with the
 * JIT's software reservation as its exclusive monitor; the compiler routes
 * only the memory and exclusive classes, the vector memory forms the inline
 * path declines and the library forms of a32_operations.hpp to it. */
extern "C" std::uint64_t uml_arm64_jit_a32_execute(
    CpuState *cpu, ::uml::memory::Tlb *tlb, std::uint32_t word,
    std::uint64_t key, std::uint32_t itstate) noexcept;

} // namespace uml::cpu::arm64
