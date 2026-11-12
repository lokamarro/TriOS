// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The encoders and closing cases the GPR-base and SP-base scalar memory-run
 * runtime tests share. Each test names its base register, the registers its
 * closing cases transfer through, and their generations and guest addresses
 * in a ScalarBase. */

#include "arm64_base_immediate_memory_test_support.hpp"
#include "arm64_engine_test_support.hpp"
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_jit_memory_run_runtime_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;
namespace immediate = uml::emulator::tests::base_immediate_memory;

inline constexpr auto page_size = std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE};
inline constexpr auto data_base = engine::engine_test_data_base;
inline constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_x(unsigned left, unsigned right) noexcept {
  return 0xeb00'001fU | ((right & 31U) << 16U) | ((left & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
load_exclusive_byte(unsigned base, unsigned transfer) noexcept {
  return 0x085f'7c00U | ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
store_exclusive_byte(unsigned status, unsigned base,
                     unsigned transfer) noexcept {
  return 0x0800'7c00U | ((status & 31U) << 16U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

struct ScalarBase final {
  unsigned reg; // 31 is SP
  std::array<unsigned, 2> remap_transfers;
  std::array<unsigned, 2> tracked_transfers;
  unsigned long remap_generation;
  std::uint64_t remap_base;
  unsigned long tracked_generation; // the first of twelve
  std::uint64_t tracked_base;
};

inline void set_base(arm64::CpuState &cpu, const ScalarBase &scalar,
                     std::uint64_t value) {
  if (scalar.reg == 31U)
    cpu.sp = value;
  else
    cpu.registers[scalar.reg] = value;
}

struct Run final {
  ::uml::cpu::EngineEvent event{};
  std::size_t executed{};
};

[[nodiscard]] inline Run run_once(uml::cpu::arm64::JitExecutor &executor,
                                  uml::memory::StaticGuestMemory &memory,
                                  unsigned long generation,
                                  arm64::CpuState &cpu) {
  engine::require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  const auto event = executor.run_block(cpu, memory, generation, executed);
  executor.end_execution();
  return {event, executed};
}

inline void remapped_page_uses_new_backing(const ScalarBase &scalar) {
  const auto generation = scalar.remap_generation;
  const auto base = scalar.remap_base;
  const auto [first, second] = scalar.remap_transfers;
  constexpr std::uint64_t old_first = 0x1111'2222'3333'4444ULL;
  constexpr std::uint64_t old_second = 0x5555'6666'7777'8888ULL;
  constexpr std::uint64_t new_first = 0x9999'aaaa'bbbb'ccccULL;
  constexpr std::uint64_t new_second = 0xdddd'eeee'ffff'0001ULL;
  engine::JitBacking original{2};
  engine::JitBacking replacement{1};
  original.put32(0, immediate::encode_unsigned(3, 1, 0, scalar.reg, first));
  original.put32(4, immediate::encode_unsigned(3, 1, 1, scalar.reg, second));
  original.put32(8, system_call);
  original.put64(page_size, old_first);
  original.put64(page_size + 8U, old_second);
  replacement.put64(0, new_first);
  replacement.put64(8, new_second);
  engine::JitMapping mapping{generation, base, original, 2};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  set_base(cpu, scalar, base + page_size);

  auto run = run_once(executor, memory, generation, cpu);
  engine::require(run.event == ::uml::cpu::EngineEvent::system_call);
  engine::require(run.executed == 3);
  engine::require(cpu.registers[first] == old_first &&
                  cpu.registers[second] == old_second);

  mapping.replace_page(base + page_size, replacement);
  cpu.pc = base;
  cpu.registers[first] = 0;
  cpu.registers[second] = 0;
  run = run_once(executor, memory, generation, cpu);
  engine::require(run.event == ::uml::cpu::EngineEvent::system_call);
  engine::require(run.executed == 3);
  engine::require(cpu.registers[first] == new_first &&
                  cpu.registers[second] == new_second);
}

enum class ReservationTarget : unsigned {
  first,
  second,
  nonoverlap,
};

inline void tracked_store_reservation_case(const ScalarBase &scalar,
                                           unsigned size_shift,
                                           ReservationTarget target) {
  const auto case_index =
      size_shift * 3U + static_cast<unsigned>(target);
  const auto generation = scalar.tracked_generation + case_index;
  const auto base = scalar.tracked_base + case_index * 4U * page_size;
  const auto [first_transfer, second_transfer] = scalar.tracked_transfers;
  constexpr std::size_t first_offset = 16;
  constexpr std::size_t second_offset = 32;
  constexpr std::size_t nonoverlap_offset = 64;
  const auto bytes = std::size_t{1} << size_shift;
  const auto reservation_offset =
      target == ReservationTarget::first
          ? first_offset + bytes - 1U
      : target == ReservationTarget::second
          ? second_offset + bytes - 1U
          : nonoverlap_offset;
  constexpr std::uint64_t first_value = 0x0123'4567'89ab'cda5ULL;
  constexpr std::uint64_t second_value = 0xfedc'ba98'7654'325aULL;
  constexpr std::uint64_t exclusive_value = 0xc7U;
  engine::JitBacking backing{2};
  backing.put32(0, load_exclusive_byte(1, 5));
  backing.put32(4, system_call);
  backing.put32(
      8, immediate::encode_unsigned(size_shift, 0,
                                    first_offset >> size_shift, scalar.reg,
                                    first_transfer));
  backing.put32(
      12, immediate::encode_unsigned(size_shift, 0,
                                     second_offset >> size_shift,
                                     scalar.reg, second_transfer));
  backing.put32(16, system_call);
  backing.put32(20, store_exclusive_byte(6, 1, 4));
  backing.put32(24, system_call);
  backing.put64(page_size + first_offset, 0);
  backing.put64(page_size + second_offset, 0);
  backing.put64(page_size + nonoverlap_offset, 0);
  engine::JitMapping mapping{generation, base, backing, 2};

  uml::cpu::arm64::JitExecutor first;
  uml::cpu::arm64::JitExecutor second;
  uml::memory::StaticGuestMemory shared_memory;
  arm64::CpuState first_cpu{};
  arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = base + page_size + reservation_offset;
  first_cpu.registers[4] = exclusive_value;
  second_cpu.pc = base + 8U;
  set_base(second_cpu, scalar, base + page_size);
  second_cpu.registers[first_transfer] = first_value;
  second_cpu.registers[second_transfer] = second_value;

  engine::require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(executed == 1);
  engine::require(first_cpu.exclusive_size == 1);

  engine::require(second.begin_execution(generation, second_cpu.pc));
  engine::require(second.run_block(second_cpu, shared_memory, generation,
                                   executed) ==
                  ::uml::cpu::EngineEvent::system_call);
  engine::require(executed == 3);
  const auto value_mask =
      bytes == sizeof(std::uint64_t)
          ? ~std::uint64_t{}
          : (std::uint64_t{1} << (bytes * 8U)) - 1U;
  engine::require((backing.get64(page_size + first_offset) & value_mask) ==
                  (first_value & value_mask));
  engine::require((backing.get64(page_size + second_offset) & value_mask) ==
                  (second_value & value_mask));
  second.end_execution();

  first_cpu.pc = base + 20U;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(executed == 1);
  const bool overlaps = target != ReservationTarget::nonoverlap;
  engine::require(first_cpu.registers[6] == (overlaps ? 1U : 0U));
  engine::require(first_cpu.exclusive_size == 0);
  if (!overlaps)
    engine::require(
        (backing.get64(page_size + nonoverlap_offset) & 0xffU) ==
        exclusive_value);
  first.end_execution();
}

inline void tracked_store_runs_track_every_range(const ScalarBase &scalar) {
  for (unsigned size_shift = 0; size_shift < 4U; ++size_shift)
    for (const auto target : {ReservationTarget::first,
                              ReservationTarget::second,
                              ReservationTarget::nonoverlap})
      tracked_store_reservation_case(scalar, size_shift, target);
}

/* Ends a test: every harness case ran on both engines with none skipped,
 * then the cases above drive a JitExecutor directly. */
inline void finish(const engine::EngineHarness &harness,
                   std::string_view suite, const ScalarBase &scalar) {
  const auto &counts = harness.counts();
  engine::require(counts.gadgets != 0);
  engine::require(counts.jit == counts.gadgets);
  engine::require(counts.jit_skipped == 0);
  engine::require(counts.jit_skipped_host_feature == 0);
  engine::require(counts.jit_unavailable == 0);
  harness.print_summary(suite);
  remapped_page_uses_new_backing(scalar);
  tracked_store_runs_track_every_range(scalar);
}

} // namespace arm64_jit_memory_run_runtime_test
