// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A memory run whose accessed bytes straddle two pages at run time. Each
 * case maps a code page and two data pages, fills both data pages'
 * translations with single accesses through x0 and x1, and then runs the
 * run in the same block. With both translations cached, a run whose every
 * access lies on one page must retire the whole block in one dispatch; a
 * run with an access that itself crosses the boundary leaves the block
 * early. Either way the run's result must match the architecture. */

#include "jit_test_support.hpp"
#include "require.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_jit_memory_run_straddle_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

inline constexpr auto page_size = std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE};
inline constexpr std::uint32_t system_call = 0xd400'0001U;

/* LDR/STR with a scaled unsigned offset. */
[[nodiscard]] constexpr std::uint32_t
load_x(unsigned transfer, unsigned base, unsigned units) noexcept {
  return 0xf940'0000U | ((units & 0xfffU) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
store_x(unsigned transfer, unsigned base, unsigned units) noexcept {
  return 0xf900'0000U | ((units & 0xfffU) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
load_d(unsigned transfer, unsigned base, unsigned units) noexcept {
  return 0xfd40'0000U | ((units & 0xfffU) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
load_q(unsigned transfer, unsigned base, unsigned units) noexcept {
  return 0x3dc0'0000U | ((units & 0xfffU) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
store_q(unsigned transfer, unsigned base, unsigned units) noexcept {
  return 0x3d80'0000U | ((units & 0xfffU) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

/* The data byte at `offset` from the first data page, before the run. */
[[nodiscard]] constexpr std::byte pattern(std::uint64_t offset) noexcept {
  return static_cast<std::byte>((offset * 37U + 11U) & 0xffU);
}

[[nodiscard]] constexpr std::uint64_t
pattern_x(std::uint64_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index)
    value |= static_cast<std::uint64_t>(pattern(offset + index))
             << (8U * index);
  return value;
}

struct Case final {
  std::string_view name;
  unsigned long generation;
  /* The code page; the data pages follow it. */
  std::uint64_t guest;
  std::span<const std::uint32_t> run;
  /* Fill write translations, with stores of XZR, rather than reads. */
  bool store{};
  /* Every access lies on one page: the block retires in one dispatch. */
  bool native{true};
};

/* The first data page's guest address. */
[[nodiscard]] constexpr std::uint64_t data(const Case &test_case) noexcept {
  return test_case.guest + page_size;
}

/* Runs the case from `cpu`, whose run registers the caller has set, to its
 * system call, and calls check(cpu, backing) with the final state and the
 * data pages' backing (offset 0 is the first data page). */
template <typename Check>
void check(const Case &test_case, arm64::CpuState cpu, Check &&verify) {
  engine::JitBacking backing{3};
  std::size_t word = 0;
  const auto put = [&](std::uint32_t instruction) {
    backing.put32(word, instruction);
    word += sizeof(std::uint32_t);
  };
  /* The fill accesses avoid the bytes within a few cache lines of the
   * boundary, which the runs use. */
  put(test_case.store ? store_x(31, 0, 0) : load_x(6, 0, 0));
  put(test_case.store ? store_x(31, 1, 32) : load_x(7, 1, 32));
  for (const auto instruction : test_case.run)
    put(instruction);
  put(system_call);
  const auto block = word / sizeof(std::uint32_t);
  std::vector<std::byte> bytes(2U * page_size);
  for (std::size_t offset = 0; offset < bytes.size(); ++offset)
    bytes[offset] = pattern(offset);
  backing.put(page_size, bytes);

  engine::JitMapping mapping{test_case.generation, test_case.guest, backing,
                             3};
  arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  cpu.pc = test_case.guest;
  cpu.registers[0] = data(test_case);
  cpu.registers[1] = data(test_case) + page_size;

  std::size_t first_dispatch = 0;
  bool finished = false;
  for (unsigned dispatch = 0; dispatch < 16U && !finished; ++dispatch) {
    engine::require(executor.begin_execution(test_case.generation, cpu.pc));
    std::size_t executed = 0;
    const auto event =
        executor.run_block(cpu, memory, test_case.generation, executed);
    executor.end_execution();
    if (dispatch == 0U)
      first_dispatch = executed;
    finished = event == ::uml::cpu::EngineEvent::system_call;
    engine::require(finished || event == ::uml::cpu::EngineEvent::continued,
                    test_case.name);
  }
  engine::require(finished, test_case.name);
  engine::require(cpu.pc == test_case.guest + word, test_case.name);
  engine::require(test_case.native ? first_dispatch == block
                                   : first_dispatch < block,
                  test_case.name);
  verify(static_cast<const arm64::CpuState &>(cpu),
         [&](std::uint64_t offset) {
           return backing.get64(page_size + offset);
         });
}

} // namespace arm64_jit_memory_run_straddle_test
