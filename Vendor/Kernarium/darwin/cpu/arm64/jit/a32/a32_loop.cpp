// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_loop.hpp"

#include "cpu/arm64/aarch32/breakpoint_immediate.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

namespace {

using ::uml::cpu::EngineEvent;
namespace memory = ::uml::memory;

/* What the shared entry loop (engine_execution.hpp) takes from an AArch32
 * task of the JIT: cpu.pc is a key for the whole entry, the host loads the
 * task's D registers and FPSCR before it, and a fault is reported at its
 * architectural 32-bit address. */
struct JitA32Entry : detail::EntryDefaults {
  [[nodiscard]] static constexpr std::uint64_t guest_pc(
      std::uint64_t key) noexcept {
    return aarch32::key_pc(key);
  }

  /* The PC key is checked at the block boundary, as the A64 entry checks
   * its alignment: bits 1:0 of a valid key are clear, so a set bit is an
   * A32 PC that is not word aligned. The fault address is the PC. */
  [[nodiscard]] static constexpr bool native_pc(std::uint64_t key) noexcept {
    return aarch32::key_is_aarch32(key);
  }
  [[nodiscard]] static constexpr bool aligned_pc(std::uint64_t key) noexcept {
    return aarch32::key_is_canonical(key);
  }

  /* The host loads an AArch32 task's D registers and FPSCR before the entry
   * (they are deferred, as an A64 task's are), and the exit saves them from
   * CpuState; load them now so no block has to know whether it needs them. */
  static void prepare(JitExecutor &, CpuState &cpu) { cpu.access_fp_state(); }

  [[nodiscard]] static EngineEvent run_block(
      JitExecutor &executor, CpuState &cpu, memory::StaticGuestMemory &guest,
      unsigned long generation, std::size_t &instructions) {
    return executor.run_a32_block(cpu, guest, generation, instructions);
  }

  [[nodiscard]] static std::uint32_t breakpoint_syndrome(
      JitExecutor &, const ExecutionRequest &request,
      memory::StaticGuestMemory &guest) {
    return aarch32::read_breakpoint_immediate(guest, request.cpu.pc);
  }
};

} // namespace

ExecutionResult run_a32_loop(JitExecutor &executor, ExecutionRequest &request) {
  return detail::run_translated_entry<JitA32Entry>(executor, request);
}

} // namespace uml::cpu::arm64::jit::a32
