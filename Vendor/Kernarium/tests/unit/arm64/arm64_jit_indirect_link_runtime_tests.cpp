// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
branch_link_register(unsigned source) noexcept {
  return 0xd63f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
return_register(unsigned source = 30) noexcept {
  return 0xd65f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
move_immediate(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
adr(unsigned destination, unsigned immediate) noexcept {
  return 0x1000'0000U | ((immediate & 3U) << 29U) |
         (((immediate >> 2U) & 0x7ffffU) << 5U) |
         (destination & 31U);
}

struct Run final {
  Event event{};
  std::size_t executed{};
};

[[nodiscard]] Run run_block(JitExecutor &executor, StaticGuestMemory &memory,
                            unsigned long generation, arm64::CpuState &cpu) {
  std::size_t executed = 0;
  const auto event =
      executor.run_block(cpu, memory, generation, executed);
  return {event, executed};
}

void write_mapped32(unsigned long generation, std::uint64_t address,
                    std::uint32_t value) {
  void *destination = nullptr;
  require(darwin_uml_static_vm_translate(
              generation, address, DARWIN_UML_STATIC_WRITE, &destination) ==
          DARWIN_UML_STATIC_TRANSLATED);
  std::memcpy(destination, &value, sizeof(value));
}

void monomorphic_and_bimorphic_sites_chain() {
  constexpr unsigned long generation = 8201;
  constexpr std::uint64_t base = 0xc0'0000;
  constexpr std::uint64_t first = base + 0x100;
  constexpr std::uint64_t second = base + 0x200;
  constexpr std::uint64_t third = base + 0x300;
  JitBacking backing{1};
  backing.put32(0, branch_register(5));
  backing.put32(first - base, move_immediate(0, 11));
  backing.put32(first - base + 4, system_call);
  backing.put32(second - base, move_immediate(0, 22));
  backing.put32(second - base + 4, system_call);
  backing.put32(third - base, move_immediate(0, 33));
  backing.put32(third - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = first;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == first);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 2);
  require(cpu.registers[0] == 11);

  cpu.pc = base;
  cpu.registers[5] = first;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 3);
  require(cpu.registers[0] == 11);

  cpu.pc = base;
  cpu.registers[5] = second;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == second);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 2);
  require(cpu.registers[0] == 22);

  for (const auto [target, expected] :
       std::array<std::pair<std::uint64_t, std::uint64_t>, 2>{
           {{first, 11}, {second, 22}}}) {
    cpu.pc = base;
    cpu.registers[5] = target;
    cpu.registers[0] = 0;
    run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::system_call);
    require(run.executed == 3);
    require(cpu.registers[0] == expected);
  }

  /* The third target replaces way zero; way one must remain a hot hit. */
  cpu.pc = base;
  cpu.registers[5] = third;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(cpu.registers[0] == 33);

  cpu.pc = base;
  cpu.registers[5] = second;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 3);
  require(cpu.registers[0] == 22);

  cpu.pc = base;
  cpu.registers[5] = first;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == first);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 4);
  require(diagnostics.trains == 3);
  executor.end_execution();
}

void resident_blr_x30_and_ret_chain_correctly() {
  constexpr unsigned long generation = 8202;
  constexpr std::uint64_t base = 0xc4'0000;
  constexpr auto callee = base + 16;
  constexpr auto continuation = base + 8;
  JitBacking backing{1};
  backing.put32(0, adr(30, callee - base));
  backing.put32(4, branch_link_register(30));
  backing.put32(8, system_call);
  backing.put32(16, return_register());
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 2);
  require(cpu.pc == callee);
  require(cpu.registers[30] == continuation);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == continuation);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 1);

  cpu.pc = base;
  cpu.registers[30] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 4);
  require(cpu.registers[30] == continuation);
  require(cpu.pc == continuation + 4);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 2);
  require(diagnostics.trains == 2);
  executor.end_execution();
}

void target_mutation_deactivates_and_retrains_site() {
  constexpr unsigned long generation = 8203;
  constexpr std::uint64_t base = 0xc8'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto target = base + page;
  JitBacking backing{2};
  backing.put32(0, branch_register(5));
  backing.put32(page, move_immediate(0, 1));
  backing.put32(page + 4, system_call);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::continued);
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = base;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  write_mapped32(generation, target, move_immediate(0, 2));
  darwin_uml_static_vm_invalidate_code(target);
  cpu.pc = base;
  cpu.registers[0] = 0;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == target);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(cpu.registers[0] == 2);
  cpu.pc = base;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 3);
  require(cpu.registers[0] == 2);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 2);
  require(diagnostics.trains == 2);
  require(diagnostics.deactivated_sites >= 1);
  executor.end_execution();
}

void permission_loss_and_unmapped_host_value_never_branch_natively() {
  constexpr unsigned long generation = 8204;
  constexpr std::uint64_t base = 0xcc'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto target = base + page;
  JitBacking backing{2};
  backing.put32(0, branch_register(5));
  backing.put32(page, system_call);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::continued);
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = base;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  executor.end_execution();

  require(darwin_uml_static_vm_protect(
              reinterpret_cast<void *>(target), page,
              PROT_READ | PROT_WRITE) == 0);
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault);
  require(run.executed == 0);
  executor.end_execution();

  require(darwin_uml_static_vm_protect(
              reinterpret_cast<void *>(target), page,
              PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
  cpu.pc = base;
  cpu.registers[5] = reinterpret_cast<std::uintptr_t>(&cpu);
  require(executor.begin_execution(generation, cpu.pc));
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == reinterpret_cast<std::uintptr_t>(&cpu));
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault);
  require(run.executed == 0);
  executor.end_execution();
}

void unsafe_target_is_never_trained() {
  constexpr unsigned long generation = 8205;
  constexpr std::uint64_t base = 0xd0'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto target = base + page;
  const auto data = target + 64;
  JitBacking backing{2};
  backing.put32(0, branch_register(5));
  backing.put32(page, 0xc85f'7c23U); // ldxr x3, [x1]
  backing.put32(page + 4, system_call);
  backing.put64(page + 64, 0x1234);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[5] = target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::continued);
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == target + 4);
  cpu.pc = base;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == target);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 2);
  require(diagnostics.trains == 0);
  require(diagnostics.unsafe_target_refusals == 1);
  executor.end_execution();
}

void stateful_target_trains_and_links_with_matching_capability() {
  constexpr unsigned long generation = 8208;
  constexpr std::uint64_t base = 0xd2'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, 0x4ea1'8400U); // add v0.4s, v0.4s, v1.4s
  backing.put32(4, branch_register(5));
  backing.put32(target - base,
                0x4ea1'8442U); // add v2.4s, v2.4s, v1.4s
  backing.put32(target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::continued);
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 2);
  cpu.pc = base;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 4);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 1);
  require(diagnostics.trains == 1);
  require(diagnostics.unsafe_target_refusals == 0);
  require(cpu.fp_loaded && cpu.fp_dirty);
  executor.end_execution();
}

void trained_target_with_missing_native_environment_falls_back() {
  constexpr unsigned long generation = 8209;
  constexpr std::uint64_t base = 0xd3'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, 0x4ea1'8400U); // add v0.4s, v0.4s, v1.4s
  backing.put32(4, branch_register(5));
  backing.put32(target - base,
                0x1e61'2842U); // fadd d2, d2, d1
  backing.put32(target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = target;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);

  cpu.pc = base;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  require(cpu.pc == target);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 1);
  require(diagnostics.trains == 1);
  require(diagnostics.unsafe_target_refusals == 0);
  arm64::restore_native_fp_environment(cpu);
  executor.end_execution();
}

void pending_training_does_not_cross_task_boundary() {
  constexpr unsigned long generation = 8206;
  constexpr std::uint64_t base = 0xd4'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, branch_register(5));
  backing.put32(target - base, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState first{};
  first.pc = base;
  first.registers[5] = target;

  require(executor.begin_execution(generation, first.pc));
  require(run_block(executor, memory, generation, first).event ==
          ::uml::cpu::EngineEvent::continued);
  executor.end_execution();

  arm64::CpuState second{};
  second.pc = target;
  require(executor.begin_execution(generation, second.pc));
  require(run_block(executor, memory, generation, second).event ==
          ::uml::cpu::EngineEvent::system_call);
  executor.end_execution();
  require(executor.indirect_link_diagnostics().trains == 0);

  first.pc = base;
  require(executor.begin_execution(generation, first.pc));
  const auto run = run_block(executor, memory, generation, first);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(first.pc == target);
  executor.end_execution();
}

void trained_sites_remain_scoped_to_their_address_space() {
  constexpr unsigned long first = 8310;
  constexpr unsigned long second = 8311;
  constexpr std::uint64_t base = 0xe8'0000;
  constexpr auto target = base + 0x100;
  JitBacking first_backing{1};
  JitBacking second_backing{1};
  first_backing.put32(0, branch_register(5));
  first_backing.put32(target - base, move_immediate(0, 11));
  first_backing.put32(target - base + 4, system_call);
  second_backing.put32(0, branch_register(5));
  second_backing.put32(target - base, move_immediate(0, 22));
  second_backing.put32(target - base + 4, system_call);
  JitMapping first_mapping{first, base, first_backing, 1};
  JitMapping second_mapping{second, base, second_backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  const auto run = [&](unsigned long generation, bool linked,
                       unsigned expected) {
    require(darwin_uml_static_vm_activate(generation) == 0);
    cpu.pc = base;
    cpu.registers[5] = target;
    cpu.registers[0] = 0;
    require(executor.begin_execution(generation, cpu.pc));
    auto result = run_block(executor, memory, generation, cpu);
    if (linked) {
      require(result.event == ::uml::cpu::EngineEvent::system_call);
      require(result.executed == 3);
    } else {
      require(result.event == ::uml::cpu::EngineEvent::continued);
      require(result.executed == 1);
      result = run_block(executor, memory, generation, cpu);
      require(result.event == ::uml::cpu::EngineEvent::system_call);
      require(result.executed == 2);
    }
    require(cpu.registers[0] == expected);
    executor.end_execution();
  };
  run(first, false, 11);
  run(second, false, 22);
  run(first, true, 11);
  run(second, true, 22);
  require(executor.indirect_link_diagnostics().deactivated_sites == 0);

  require(darwin_uml_static_vm_activate(first) == 0);
  write_mapped32(first, target, move_immediate(0, 33));
  darwin_uml_static_vm_invalidate_code(target);
  run(second, true, 22);
  run(first, false, 33);
  run(first, true, 33);
  require(executor.indirect_link_diagnostics().deactivated_sites == 1);
}

/* The checkpoint stores the target PC. With top-byte ignore the matched
 * record's key also carries a state bit, which is not part of a guest PC. */
void indirect_cycles_obey_fairness_checkpoint(bool top_byte_ignore) {
  const unsigned long generation = top_byte_ignore ? 8227 : 8207;
  constexpr std::uint64_t base = 0xd8'0000;
  constexpr auto other = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, branch_register(5));
  backing.put32(other - base, branch_register(6));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.tbi_enabled = top_byte_ignore;
  cpu.registers[5] = other;
  cpu.registers[6] = base;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == other);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 1);
  require(cpu.pc == base);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed ==
          uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
  require(cpu.pc == base);
  const auto diagnostics = executor.indirect_link_diagnostics();
  require(diagnostics.misses == 2);
  require(diagnostics.trains == 2);
  executor.end_execution();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  monomorphic_and_bimorphic_sites_chain();
  resident_blr_x30_and_ret_chain_correctly();
  target_mutation_deactivates_and_retrains_site();
  permission_loss_and_unmapped_host_value_never_branch_natively();
  unsafe_target_is_never_trained();
  stateful_target_trains_and_links_with_matching_capability();
  trained_target_with_missing_native_environment_falls_back();
  pending_training_does_not_cross_task_boundary();
  trained_sites_remain_scoped_to_their_address_space();
  indirect_cycles_obey_fairness_checkpoint(false);
  indirect_cycles_obey_fairness_checkpoint(true);
  return 0;
}
