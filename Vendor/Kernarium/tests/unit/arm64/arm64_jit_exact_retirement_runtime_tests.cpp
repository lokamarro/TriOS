// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t load_pair_q0_q1_x0 = 0xad40'0400U;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t breakpoint = 0xd420'0000U;
constexpr std::uint32_t illegal = 0xffff'ffffU;

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x1400'0000U |
         (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

struct Run final {
  Event event{};
  std::size_t executed{};
};

[[nodiscard]] Run run(JitExecutor &executor, StaticGuestMemory &memory,
                      unsigned long generation, arm64::CpuState &cpu) {
  std::size_t executed = 99;
  const auto event =
      executor.run_block(cpu, memory, generation, executed);
  return {event, executed};
}

void direct_constant_terminal(unsigned long generation, std::uint64_t base,
                              std::uint32_t terminal, Event expected_event,
                              std::size_t target_retired) {
  constexpr std::uint64_t target_offset = 0x40;
  const auto target = base + target_offset;
  JitBacking backing{1};
  backing.put32(0, branch(base, target));
  backing.put32(target_offset, terminal);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = target;
  require(executor.begin_execution(generation, cpu.pc));

  auto result = run(executor, memory, generation, cpu);
  require(result.event == expected_event);
  require(result.executed == target_retired);

  cpu.pc = base;
  result = run(executor, memory, generation, cpu);
  require(result.event == expected_event);
  require(result.executed == 1U + target_retired);
  executor.end_execution();
}

void pic_constant_terminal(unsigned long generation, std::uint64_t base,
                           std::uint32_t terminal, Event expected_event,
                           std::size_t target_retired) {
  constexpr std::uint64_t target_offset = 0x40;
  const auto target = base + target_offset;
  JitBacking backing{1};
  backing.put32(0, branch_register(5));
  backing.put32(target_offset, terminal);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = target;
  require(executor.begin_execution(generation, cpu.pc));

  auto result = run(executor, memory, generation, cpu);
  require(result.event == ::uml::cpu::EngineEvent::continued);
  require(result.executed == 1);
  require(cpu.pc == target);

  result = run(executor, memory, generation, cpu);
  require(result.event == expected_event);
  require(result.executed == target_retired);

  cpu.pc = base;
  result = run(executor, memory, generation, cpu);
  require(result.event == expected_event);
  require(result.executed == 1U + target_retired);
  executor.end_execution();
}

void direct_and_pic_constant_terminals_are_exact() {
  direct_constant_terminal(8600, 0x5c00'0000, system_call,
                           ::uml::cpu::EngineEvent::system_call, 1);
  direct_constant_terminal(8601, 0x5c10'0000, breakpoint,
                           ::uml::cpu::EngineEvent::breakpoint, 1);
  direct_constant_terminal(8602, 0x5c20'0000, illegal,
                           ::uml::cpu::EngineEvent::illegal_instruction, 0);
  pic_constant_terminal(8603, 0x5c30'0000, system_call,
                        ::uml::cpu::EngineEvent::system_call, 1);
  pic_constant_terminal(8604, 0x5c40'0000, breakpoint,
                        ::uml::cpu::EngineEvent::breakpoint, 1);
  pic_constant_terminal(8605, 0x5c50'0000, illegal,
                        ::uml::cpu::EngineEvent::illegal_instruction, 0);
}

void dynamic_success_with_odd_prefix_is_exact() {
  constexpr unsigned long generation = 8606;
  constexpr std::uint64_t base = 0x5c60'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::array code{
      add_x5_one,
      load_pair_q0_q1_x0,
      system_call,
  };
  std::array<std::byte, 32> data{};
  for (std::size_t index = 0; index < data.size(); ++index)
    data[index] = static_cast<std::byte>(index + 1U);

  JitBacking backing{2};
  for (std::size_t index = 0; index < code.size(); ++index)
    backing.put32(index * sizeof(std::uint32_t), code[index]);
  backing.put(page - 16U, data);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = base + page - 16U;
  cpu.registers[5] = 41;
  require(executor.begin_execution(generation, cpu.pc));

  const auto result = run(executor, memory, generation, cpu);
  require(result.event == ::uml::cpu::EngineEvent::continued);
  require(result.executed == 2);
  require(cpu.pc == base + 2U * sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  executor.end_execution();
}

void dynamic_fault_with_odd_prefix_is_exact() {
  constexpr unsigned long generation = 8607;
  constexpr std::uint64_t base = 0x5c70'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::array code{
      add_x5_one,
      load_pair_q0_q1_x0,
      system_call,
  };
  JitBacking backing{1};
  for (std::size_t index = 0; index < code.size(); ++index)
    backing.put32(index * sizeof(std::uint32_t), code[index]);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = base + page - 16U;
  cpu.registers[5] = 41;
  require(executor.begin_execution(generation, cpu.pc));

  const auto result = run(executor, memory, generation, cpu);
  require(result.event == ::uml::cpu::EngineEvent::memory_fault);
  require(result.executed == 1);
  require(cpu.pc == base + sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  require(executor.fault_address() == base + page);
  executor.end_execution();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  direct_and_pic_constant_terminals_are_exact();
  dynamic_success_with_odd_prefix_is_exact();
  dynamic_fault_with_odd_prefix_is_exact();
  return 0;
}
