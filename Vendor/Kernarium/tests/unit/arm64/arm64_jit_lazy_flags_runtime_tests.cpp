// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace uml::emulator::tests;

namespace {

using Access = uml::memory::Access;
using Event = ::uml::cpu::EngineEvent;

[[nodiscard]] Event run_once(uml::cpu::arm64::JitExecutor &executor,
                             uml::cpu::arm64::CpuState &cpu,
                             uml::memory::StaticGuestMemory &memory,
                             unsigned long generation,
                             std::size_t &executed) {
  require(executor.begin_execution(generation, cpu.pc));
  const auto event = executor.run_block(cpu, memory, generation, executed);
  executor.end_execution();
  return event;
}

void returning_tlb_helper_preserves_guest_flags() {
  constexpr unsigned long generation = 710;
  constexpr std::uint64_t base = 0x85'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint64_t value = 0x0123'4567'89ab'cdefULL;
  JitBacking backing{2};
  backing.put32(0, 0xeb02'003fU);  // cmp x1, x2
  backing.put32(4, 0x9a86'00a4U);  // csel x4, x5, x6, eq
  backing.put32(8, 0xf940'0003U);  // ldr x3, [x0]
  backing.put32(12, 0x9a1f'03e7U); // adc x7, xzr, xzr
  backing.put32(16, 0xd400'0001U); // svc #0
  backing.put64(page, value);
  JitMapping mapping{generation, base, backing, 2};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = base + page;
  cpu.registers[1] = 0x55;
  cpu.registers[2] = 0x55;
  cpu.registers[5] = 0x1111;
  cpu.registers[6] = 0x2222;
  cpu.nzcv = 0x9000'0000U;

  std::size_t executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 5);
  require(cpu.pc == base + 20);
  require(cpu.registers[3] == value);
  require(cpu.registers[4] == 0x1111);
  require(cpu.registers[7] == 1);
  require(cpu.nzcv == 0x6000'0000U);

  /* Re-enter with the same generation and address so the data lookup hits
   * the warmed TLB entry. The guest condition must survive that shorter path
   * just as it survived the returning miss bridge above. */
  cpu.pc = base;
  cpu.registers[1] = 1;
  cpu.registers[2] = 2;
  cpu.registers[3] = 0;
  cpu.registers[4] = 0;
  cpu.registers[7] = 1;
  executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 5);
  require(cpu.pc == base + 20);
  require(cpu.registers[3] == value);
  require(cpu.registers[4] == 0x2222);
  require(cpu.registers[7] == 0);
  require(cpu.nzcv == 0x8000'0000U);
}

void sp_alignment_edges_publish_exact_flags() {
  constexpr unsigned long generation = 717;
  constexpr std::uint64_t base = 0x8c'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  JitBacking backing{2};
  backing.put32(0, 0xeb02'003fU);  // cmp x1, x2
  backing.put32(4, 0xf900'03e3U);  // str x3, [sp]
  backing.put32(8, 0x9a86'00a4U);  // csel x4, x5, x6, eq
  backing.put32(12, 0xd400'0001U); // svc #0
  JitMapping mapping{generation, base, backing, 2};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.sp = base + page;
  cpu.registers[1] = 0x55;
  cpu.registers[2] = 0x55;
  cpu.registers[3] = 0x0123'4567'89ab'cdefULL;
  cpu.registers[5] = 0x1111;
  cpu.registers[6] = 0x2222;
  cpu.nzcv = 0x9000'0000U;

  std::size_t executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 4);
  require(cpu.pc == base + 16);
  require(cpu.registers[4] == 0x1111);
  require(cpu.nzcv == 0x6000'0000U);

  cpu.pc = base;
  cpu.sp = base + page + 8;
  cpu.registers[4] = 0;
  cpu.nzcv = 0x9000'0000U;
  executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::memory_fault);
  require(executed == 1);
  require(cpu.pc == base + 4);
  require(cpu.registers[4] == 0);
  require(cpu.nzcv == 0x6000'0000U);
  require(executor.fault_address() == base + page + 8);
  require(executor.fault_access() == Access::write);
}

void cold_fault_and_trap_publish_exact_flags() {
  {
    constexpr unsigned long generation = 711;
    constexpr std::uint64_t base = 0x86'0000;
    constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
    JitBacking backing{1};
    backing.put32(0, 0xeb02'003fU); // cmp x1, x2
    backing.put32(4, 0xf940'0003U); // ldr x3, [x0]
    JitMapping mapping{generation, base, backing, 1};
    uml::cpu::arm64::JitExecutor executor;
    uml::memory::StaticGuestMemory memory;
    uml::cpu::arm64::CpuState cpu{};
    cpu.pc = base;
    cpu.registers[0] = base + page;
    cpu.registers[1] = 7;
    cpu.registers[2] = 7;
    cpu.nzcv = 0x9000'0000U;

    std::size_t executed = 0;
    require(run_once(executor, cpu, memory, generation, executed) ==
            ::uml::cpu::EngineEvent::memory_fault);
    require(executed == 1);
    require(cpu.pc == base + 4);
    require(cpu.nzcv == 0x6000'0000U);
    require(executor.fault_address() == base + page);
    require(executor.fault_access() == Access::read);
  }

  {
    constexpr unsigned long generation = 712;
    constexpr std::uint64_t base = 0x87'0000;
    JitBacking backing{1};
    backing.put32(0, 0xb100'0400U); // adds x0, x0, #1
    backing.put32(4, 0xd420'0000U); // brk #0
    JitMapping mapping{generation, base, backing, 1};
    uml::cpu::arm64::JitExecutor executor;
    uml::memory::StaticGuestMemory memory;
    uml::cpu::arm64::CpuState cpu{};
    cpu.pc = base;
    cpu.registers[0] = ~std::uint64_t{};
    cpu.nzcv = 0x9000'0000U;

    std::size_t executed = 0;
    require(run_once(executor, cpu, memory, generation, executed) ==
            ::uml::cpu::EngineEvent::breakpoint);
    require(executed == 2);
    require(cpu.pc == base + 4);
    require(cpu.registers[0] == 0);
    require(cpu.nzcv == 0x6000'0000U);
  }
}

void system_register_read_and_write_synchronize_lazy_flags() {
  constexpr unsigned long generation = 713;
  constexpr std::uint64_t base = 0x88'0000;
  JitBacking backing{1};
  backing.put32(0, 0xeb02'003fU);  // cmp x1, x2
  backing.put32(4, 0xd53b'4203U);  // mrs x3, nzcv
  backing.put32(8, 0xd51b'4204U);  // msr nzcv, x4
  backing.put32(12, 0x9a87'40c5U); // csel x5, x6, x7, mi
  backing.put32(16, 0xd400'0001U); // svc #0
  JitMapping mapping{generation, base, backing, 1};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = 9;
  cpu.registers[2] = 9;
  cpu.registers[4] = 0x9000'0000U;
  cpu.registers[6] = 0xaaaa;
  cpu.registers[7] = 0xbbbb;

  std::size_t executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 5);
  require(cpu.pc == base + 20);
  require(cpu.registers[3] == 0x6000'0000U);
  require(cpu.registers[5] == 0xaaaa);
  require(cpu.nzcv == 0x9000'0000U);
}

void internal_control_test_does_not_replace_guest_flags() {
  constexpr unsigned long generation = 716;
  constexpr std::uint64_t base = 0x8b'0000;
  JitBacking backing{1};
  backing.put32(0, 0xeb02'003fU);  // cmp x1, x2
  backing.put32(4, 0xd51b'4403U);  // msr fpcr, x3
  backing.put32(8, 0x9a86'00a4U);  // csel x4, x5, x6, eq
  backing.put32(12, 0xd400'0001U); // svc #0
  JitMapping mapping{generation, base, backing, 1};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = 0x1234;
  cpu.registers[2] = 0x1234;
  cpu.registers[3] = 0;
  cpu.registers[5] = 0xaaaa;
  cpu.registers[6] = 0xbbbb;
  cpu.nzcv = 0x9000'0000U;

  std::size_t executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 4);
  require(cpu.pc == base + 16);
  require(cpu.registers[4] == 0xaaaa);
  require(cpu.nzcv == 0x6000'0000U);
}

void linked_helper_and_invalidation_keep_guest_flags() {
  constexpr unsigned long generation = 714;
  constexpr std::uint64_t base = 0x89'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint64_t first = 0x1111'2222'3333'4444ULL;
  constexpr std::uint64_t second = 0xaaaa'bbbb'cccc'ddddULL;
  JitBacking backing{3};
  backing.put32(0, 0xeb02'003fU);  // cmp x1, x2
  backing.put32(4, 0x1400'0003U);  // b +12
  backing.put32(16, 0xf940'0003U); // ldr x3, [x0]
  backing.put32(20, 0x9a85'0086U); // csel x6, x4, x5, eq
  backing.put32(24, 0xd400'0001U); // svc #0
  backing.put64(page, first);
  backing.put64(2 * page, second);
  JitMapping mapping{generation, base, backing, 3};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.registers[0] = base + page;
  cpu.registers[1] = 5;
  cpu.registers[2] = 5;
  cpu.registers[4] = 0x44;
  cpu.registers[5] = 0x55;
  std::size_t executed = 0;

  /* Compile the flag-producing predecessor while the target link is null. */
  cpu.pc = base;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 2);
  require(cpu.pc == base + 16);
  require(cpu.nzcv == 0x6000'0000U);

  /* Publish the target and warm only the first data-page translation. */
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 3);
  require(cpu.registers[3] == first);
  require(cpu.registers[6] == 0x44);

  /* The linked target enters after its prologue. Its second-page TLB miss and
   * internal comparisons must not replace the predecessor's guest flags. */
  cpu.pc = base;
  cpu.registers[0] = base + 2 * page;
  cpu.registers[3] = 0;
  cpu.registers[6] = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 5);
  require(cpu.registers[3] == second);
  require(cpu.registers[6] == 0x44);
  require(cpu.nzcv == 0x6000'0000U);

  /* Replace EQ with NE. The code epoch must clear the old internal target,
   * then publish and execute only the replacement while preserving NZCV. */
  void *code = nullptr;
  require(darwin_uml_static_vm_translate(
              generation, base + 20, DARWIN_UML_STATIC_WRITE, &code) ==
          DARWIN_UML_STATIC_TRANSLATED);
  const std::uint32_t replacement = 0x9a85'1086U; // csel x6,x4,x5,ne
  std::memcpy(code, &replacement, sizeof(replacement));
  darwin_uml_static_vm_invalidate_code(base + 20);

  cpu.pc = base;
  cpu.registers[0] = base + page;
  cpu.registers[6] = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 2);
  require(cpu.pc == base + 16);
  require(cpu.nzcv == 0x6000'0000U);
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 3);
  require(cpu.registers[6] == 0x55);
  require(cpu.nzcv == 0x6000'0000U);

  cpu.pc = base;
  cpu.registers[6] = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 5);
  require(cpu.registers[6] == 0x55);
  require(cpu.nzcv == 0x6000'0000U);
}

void fairness_exit_publishes_last_iteration_flags() {
  constexpr unsigned long generation = 715;
  constexpr std::uint64_t base = 0x8a'0000;
  JitBacking backing{1};
  backing.put32(0, 0xb100'0400U); // adds x0, x0, #1
  backing.put32(4, 0x17ff'ffffU); // b -4
  JitMapping mapping{generation, base, backing, 1};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  const auto chain_limit =
      uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
  cpu.pc = base;
  cpu.registers[0] =
      std::uint64_t{} - static_cast<std::uint64_t>(chain_limit / 2U);
  cpu.nzcv = 0x9000'0000U;

  std::size_t executed = 0;
  require(run_once(executor, cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == chain_limit);
  require(cpu.pc == base);
  require(cpu.registers[0] == 0);
  require(cpu.nzcv == 0x6000'0000U);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  returning_tlb_helper_preserves_guest_flags();
  sp_alignment_edges_publish_exact_flags();
  cold_fault_and_trap_publish_exact_flags();
  system_register_read_and_write_synchronize_lazy_flags();
  internal_control_test_does_not_replace_guest_flags();
  linked_helper_and_invalidation_keep_guest_flags();
  fairness_exit_publishes_last_iteration_flags();
  return 0;
}
