// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/single_copy_alignment.hpp"
#include "cpu/arm64/memory.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>

/*
 * The JIT's semantic slow path. Generated code leaves a fixed set of
 * instruction classes to the helpers in jit_semantic_memory.cpp: dynamic
 * cases its native memory lowering declines (unsupported shapes,
 * misalignment, exact fault metadata), the software exclusive monitor, and
 * the MOPS family. Each handler executes exactly one instruction of its class
 * against CpuState and a GuestMemory view of the executor's TLB.
 *
 * The class selectors recognize only the words the compiler routes to each
 * helper. Any other word has no handler, and the helper reports it as an
 * internal error instead of interpreting it.
 */

namespace uml::cpu::arm64::jit::semantic {

enum class StopReason : std::uint8_t {
  continued,
  memory_fault,
  illegal_instruction,
  unsupported_instruction,
};

struct RunResult final {
  StopReason reason{StopReason::illegal_instruction};
  std::uint64_t address{};
  std::uint32_t instruction{};
  std::size_t instructions{};
  bool write{};
  bool alignment{};
};

/* Alignment faults precede translation and must never be repaired as missing
 * pages. Preserve the effective address without retiring the instruction. */
[[nodiscard]] constexpr RunResult alignment_fault(std::uint64_t address,
                                                  std::uint32_t instruction,
                                                  bool write) noexcept {
  return {StopReason::memory_fault, address, instruction, 0, write, true};
}

/* A failed guest access at address. The instruction does not retire. */
[[nodiscard]] constexpr RunResult memory_fault(std::uint64_t address,
                                               std::uint32_t instruction,
                                               bool write) noexcept {
  return {StopReason::memory_fault, address, instruction, 0, write};
}

/* An UNDEFINED encoding at pc. The instruction does not retire. */
[[nodiscard]] constexpr RunResult illegal(std::uint64_t pc,
                                          std::uint32_t instruction) noexcept {
  return {StopReason::illegal_instruction, pc, instruction, 0, false};
}

/* The FEAT_LSE2 alignment rule for atomic, exclusive and ordered accesses,
 * under the executor's host-capability scope. */
[[nodiscard]] inline bool single_copy_alignment_faults(std::uint64_t address,
                                                       std::size_t size) noexcept {
  return arm64::detail::single_copy_alignment_faults(
      address, size,
      ::uml::cpu::host_capabilities().has(::uml::cpu::ArmFeature::lse2));
}

[[nodiscard]] constexpr RunResult continue_after(std::uint64_t address,
                                                 std::uint32_t instruction) {
  return {StopReason::continued, address, instruction, 1, false};
}

using Handler = RunResult (*)(CpuState &, GuestMemory &, std::uint32_t);

/* uml_arm64_jit_execute_memory: ordered and exact-fault memory forms.
 * FEAT_MOPS has its own helper, uml_arm64_jit_execute_mops. */
[[nodiscard]] Handler memory_handler(std::uint32_t instruction) noexcept;
/* uml_arm64_jit_execute_exclusive: exclusives, compare-and-swap, and LSE
 * atomics. */
[[nodiscard]] Handler exclusive_handler(std::uint32_t instruction) noexcept;
/* uml_arm64_jit_execute_ordered: the store half of the ordered scalar
 * load/store family, STLR. */
[[nodiscard]] Handler ordered_store_handler(std::uint32_t instruction) noexcept;

[[nodiscard]] RunResult load_store_unsigned(CpuState &, GuestMemory &,
                                            std::uint32_t);
[[nodiscard]] RunResult load_literal(CpuState &, GuestMemory &,
                                     std::uint32_t);
[[nodiscard]] RunResult load_store_other(CpuState &, GuestMemory &,
                                         std::uint32_t);
[[nodiscard]] RunResult load_store_exclusive(CpuState &, GuestMemory &,
                                             std::uint32_t);
[[nodiscard]] RunResult load_store_compare_swap(CpuState &, GuestMemory &,
                                                std::uint32_t);
[[nodiscard]] RunResult load_store_compare_swap_pair(CpuState &,
                                                     GuestMemory &,
                                                     std::uint32_t);
[[nodiscard]] RunResult load_store_atomic(CpuState &, GuestMemory &,
                                          std::uint32_t);
[[nodiscard]] RunResult load_store_rcpc(CpuState &, GuestMemory &,
                                        std::uint32_t);

} // namespace uml::cpu::arm64::jit::semantic
