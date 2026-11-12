// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace uml::emulator::tests;

extern "C" std::uint64_t
uml_arm64_jit_chain_abi_probe(void *context, std::uint64_t *observed);

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t breakpoint = 0xd420'0000U;

[[nodiscard]] constexpr std::uint32_t branch(
    std::uint64_t source, std::uint64_t target) noexcept {
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

[[nodiscard]] constexpr std::uint32_t
branch_link_register(unsigned source) noexcept {
  return 0xd63f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t branch_condition(
    std::uint64_t source, std::uint64_t target,
    unsigned condition) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x5400'0000U |
         ((static_cast<std::uint32_t>(displacement) & 0x7ffffU) << 5U) |
         (condition & 15U);
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

struct AbiProbeContext final {
  JitExecutor *executor;
  StaticGuestMemory *memory;
  arm64::CpuState *cpu;
  unsigned long generation;
};

void host_callee_saved_registers_are_restored() {
  constexpr unsigned long generation = 8300;
  constexpr std::uint64_t base = 0xdc'0000;
  JitBacking backing{1};
  backing.put32(0, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  AbiProbeContext context{&executor, &memory, &cpu, generation};
  std::array<std::uint64_t, 5> observed{};
  const auto packed =
      uml_arm64_jit_chain_abi_probe(&context, observed.data());
  require(static_cast<Event>(static_cast<std::uint32_t>(packed)) ==
          ::uml::cpu::EngineEvent::system_call);
  require((packed >> 32U) == 1);
  require(observed ==
          std::array<std::uint64_t, 5>{
              0x1919, 0x2020, 0x2121, 0x2222, 0x2323});
  executor.end_execution();
}

void direct_chain_length(std::size_t links, unsigned long generation,
                         std::uint64_t base) {
  JitBacking backing{1};
  for (std::size_t index = 0; index < links; ++index) {
    const auto pc = base + index * 16U;
    const auto target = pc + 16U;
    backing.put32(index * 16U, 0x9100'0400U); // add x0, x0, #1
    backing.put32(index * 16U + 4U, branch(pc + 4U, target));
  }
  const auto terminal = base + links * 16U;
  backing.put32(links * 16U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = terminal;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 1);
  for (std::size_t index = links; index-- > 0;) {
    cpu.pc = base + index * 16U;
    run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::system_call);
  }

  cpu.pc = base;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == links * 2U + 1U);
  require(cpu.registers[0] == links);
  executor.end_execution();
}

void zero_one_two_three_and_many_links() {
  direct_chain_length(0, 8301, 0xdd'0000);
  direct_chain_length(1, 8302, 0xde'0000);
  direct_chain_length(2, 8303, 0xdf'0000);
  direct_chain_length(3, 8304, 0xe0'0000);
  direct_chain_length(32, 8305, 0xe1'0000);
}

/* A straight run of more words than the fetch window holds is several
 * blocks, and each one's exit asks for a link to the next like a branch's,
 * so a chain carries on across them. */
void fetch_window_blocks_chain() {
  constexpr unsigned long generation = 8320;
  constexpr std::uint64_t base = 0xf0'0000;
  constexpr std::size_t window = 64;
  constexpr std::size_t words = 2U * window + 22U;
  JitBacking backing{1};
  for (std::size_t index = 0; index < words; ++index)
    backing.put32(index * 4U, 0x9100'0400U); // add x0, x0, #1
  backing.put32(words * 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base + 2U * window * 4U;
  require(executor.begin_execution(generation, cpu.pc));

  /* The last block first, so each earlier one links to a translated
   * successor. */
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == words - 2U * window + 1U);
  cpu.pc = base + window * 4U;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == words + 1U);
  require(cpu.registers[0] == words);
  executor.end_execution();
}

void exact_fairness_commit_boundary() {
  constexpr unsigned long generation = 8306;
  constexpr std::uint64_t base = 0xe2'0000;
  JitBacking backing{1};
  backing.put32(0, branch(base, base));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed ==
          uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
  require(cpu.pc == base);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  /*
   * Both invocations reach the final one-instruction block with one remaining
   * slot. Its completed branch is restored into the budget when the next
   * whole block cannot fit, so the returned count remains exactly 4096.
   */
  require(run.executed ==
          uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
  require(cpu.pc == base);
  executor.end_execution();
}

void non_dividing_fairness_quantum_restores_the_source() {
  constexpr unsigned long generation = 8310;
  constexpr std::uint64_t base = 0xe6'0000;
  constexpr std::size_t quantum = 3;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0529U); // add x9, x9, #1
  backing.put32(8, branch(base + 8, base));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  const auto limit =
      uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
  const auto expected = (limit / quantum) * quantum;
  const auto iterations = expected / quantum;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == expected);
  require(cpu.pc == base);
  require(cpu.registers[8] == iterations);
  require(cpu.registers[9] == iterations);

  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == expected);
  require(cpu.pc == base);
  require(cpu.registers[8] == iterations * 2U);
  require(cpu.registers[9] == iterations * 2U);
  executor.end_execution();
}

void direct_and_pic_links_carry_x8_x9() {
  constexpr unsigned long generation = 8311;
  constexpr std::uint64_t base = 0xe7'0000;
  constexpr auto direct_target = base + 0x40;
  constexpr auto pic_source = base + 0x80;
  constexpr auto pic_target = base + 0xc0;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, branch(base + 8, direct_target));
  backing.put32(direct_target - base,
                0x8b09'0108U); // add x8, x8, x9
  backing.put32(direct_target - base + 4, system_call);
  backing.put32(pic_source - base,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(pic_source - base + 4,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(pic_source - base + 8, branch_register(5));
  backing.put32(pic_target - base,
                0x8b09'0108U); // add x8, x8, x9
  backing.put32(pic_target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = direct_target;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[8] = 10;
  cpu.registers[9] = 20;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 5);
  require(cpu.registers[8] == 33 && cpu.registers[9] == 22);

  cpu.pc = pic_source;
  cpu.registers[5] = pic_target;
  cpu.registers[8] = 30;
  cpu.registers[9] = 40;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 3 && cpu.pc == pic_target);
  require(cpu.registers[8] == 31 && cpu.registers[9] == 42);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 2);

  cpu.pc = pic_source;
  cpu.registers[8] = 30;
  cpu.registers[9] = 40;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == 5);
  require(cpu.registers[8] == 73 && cpu.registers[9] == 42);
  executor.end_execution();
}

void carried_indirect_operands_survive_miss_and_success() {
  constexpr unsigned long generation = 8316;
  constexpr std::uint64_t base = 0xec'0000;
  constexpr auto br_target = base + 0x40;
  constexpr auto blr_source = base + 0x80;
  constexpr auto blr_target = base + 0xc0;
  JitBacking backing{1};
  backing.put32(0, branch_register(8));
  backing.put32(br_target - base,
                0x9100'0d29U); // add x9, x9, #3
  backing.put32(br_target - base + 4, system_call);
  backing.put32(blr_source - base, branch_link_register(9));
  backing.put32(blr_source - base + 4, system_call);
  backing.put32(blr_target - base,
                0x9100'1508U); // add x8, x8, #5
  backing.put32(blr_target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[8] = br_target;
  cpu.registers[9] = 10;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 1 && cpu.pc == br_target);
  require(cpu.registers[8] == br_target && cpu.registers[9] == 10);
  auto links = executor.indirect_link_diagnostics();
  require(links.misses == 1 && links.trains == 0);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 2);
  require(cpu.registers[8] == br_target && cpu.registers[9] == 13);
  links = executor.indirect_link_diagnostics();
  require(links.misses == 1 && links.trains == 1);

  cpu.pc = base;
  cpu.registers[8] = br_target;
  cpu.registers[9] = 20;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 3);
  require(cpu.registers[8] == br_target && cpu.registers[9] == 23);
  links = executor.indirect_link_diagnostics();
  require(links.misses == 1 && links.trains == 1);

  cpu.pc = blr_source;
  cpu.registers[8] = 30;
  cpu.registers[9] = blr_target;
  cpu.registers[30] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 1 && cpu.pc == blr_target);
  require(cpu.registers[8] == 30 && cpu.registers[9] == blr_target &&
          cpu.registers[30] == blr_source + sizeof(std::uint32_t));
  links = executor.indirect_link_diagnostics();
  require(links.misses == 2 && links.trains == 1);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 2);
  require(cpu.registers[8] == 35 && cpu.registers[9] == blr_target);
  links = executor.indirect_link_diagnostics();
  require(links.misses == 2 && links.trains == 2);

  cpu.pc = blr_source;
  cpu.registers[8] = 40;
  cpu.registers[9] = blr_target;
  cpu.registers[30] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 3);
  require(cpu.registers[8] == 45 && cpu.registers[9] == blr_target &&
          cpu.registers[30] == blr_source + sizeof(std::uint32_t));
  links = executor.indirect_link_diagnostics();
  require(links.misses == 2 && links.trains == 2);
  executor.end_execution();
}

void linked_helper_observes_canonical_x8_x9() {
  constexpr unsigned long generation = 8312;
  constexpr std::uint64_t base = 0xe8'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto target = base + 0x40;
  constexpr auto data = base + page - 8;
  constexpr std::uint64_t first = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t second = 0x8877'6655'4433'2211ULL;
  JitBacking backing{2};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0d29U); // add x9, x9, #3
  backing.put32(8, branch(base + 8, target));
  backing.put32(target - base, 0xa940'0500U); // ldp x0, x1, [x8]
  backing.put32(target - base + 4, system_call);
  backing.put64(page - 8, first);
  backing.put64(page, second);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = target;
  cpu.registers[8] = data;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 1);
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[8] = data - 1;
  cpu.registers[9] = 7;
  cpu.registers[0] = 0;
  cpu.registers[1] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == 4 && cpu.pc == target + 4);
  require(cpu.registers[8] == data && cpu.registers[9] == 10);
  require(cpu.registers[0] == first && cpu.registers[1] == second);
  executor.end_execution();
}

void linked_helper_writes_x8_x9_before_a_linked_consumer() {
  constexpr unsigned long generation = 8315;
  constexpr std::uint64_t base = 0xeb'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto target = base + 0x40;
  constexpr auto consumer = base + 0x80;
  constexpr auto data = base + page - 8;
  constexpr std::uint64_t first = 0x0123'4567'89ab'cdefULL;
  constexpr std::uint64_t second = 0x1111'2222'3333'4444ULL;
  JitBacking backing{2};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, branch(base + 8, target));
  backing.put32(target - base,
                0xa940'24c8U); // ldp x8, x9, [x6]
  backing.put32(target - base + 4,
                branch(target + 4, consumer));
  backing.put32(consumer - base,
                0x8b09'0108U); // add x8, x8, x9
  backing.put32(consumer - base + 4, system_call);
  backing.put64(page - 8, first);
  backing.put64(page, second);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  /* Warm the consumer and the target tail so the helper result's next edge
   * is a successful direct link rather than an outer-dispatch fallback. */
  cpu.pc = consumer;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = target + sizeof(std::uint32_t);
  cpu.registers[8] = first;
  cpu.registers[9] = second;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 3);
  require(cpu.registers[8] == first + second &&
          cpu.registers[9] == second);

  /* Execute the target once to make it linkable. The cross-page pair load
   * takes its helper and returns after writing canonical x8/x9. */
  cpu.pc = target;
  cpu.registers[6] = data;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 1 &&
          cpu.pc == target + sizeof(std::uint32_t));
  require(cpu.registers[8] == first && cpu.registers[9] == second);

  /* Three source instructions plus the target helper prove that the source
   * entered this link-safe helper-backed target through its live link. */
  cpu.pc = base;
  cpu.registers[6] = data;
  cpu.registers[8] = 0x111;
  cpu.registers[9] = 0x222;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 4 &&
          cpu.pc == target + sizeof(std::uint32_t));
  require(cpu.registers[8] == first && cpu.registers[9] == second);

  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 3);
  require(cpu.registers[8] == first + second &&
          cpu.registers[9] == second);
  executor.end_execution();
}

void continuing_tlb_helper_reloads_x8_x9() {
  constexpr unsigned long generation = 8314;
  constexpr std::uint64_t base = 0xea'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  JitBacking backing{2};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x3940'00c0U); // ldrb w0, [x6]
  backing.put32(8, 0x9100'0929U); // add x9, x9, #2
  backing.put32(12, 0x8b09'0108U); // add x8, x8, x9
  backing.put32(16, system_call);
  backing.put32(page, 0x0000'00abU);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[6] = base + page;
  cpu.registers[8] = 10;
  cpu.registers[9] = 20;
  require(executor.begin_execution(generation, cpu.pc));

  const auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 5);
  require(cpu.registers[0] == 0xab);
  require(cpu.registers[8] == 33 && cpu.registers[9] == 22);
  executor.end_execution();
}

void conditional_and_pic_exits_chain() {
  constexpr unsigned long generation = 8307;
  constexpr std::uint64_t base = 0xe3'0000;
  constexpr auto conditional_target = base + 0x40;
  constexpr auto pic_source = base + 0x80;
  constexpr auto pic_target = base + 0xc0;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, 0xf100'0400U); // subs x0, x0, #1
  backing.put32(12,
                branch_condition(base + 12, conditional_target, 1)); // NE
  backing.put32(16, system_call);
  backing.put32(conditional_target - base, system_call);
  backing.put32(pic_source - base, branch_register(5));
  backing.put32(pic_target - base, 0x9100'0442U); // add x2, x2, #1
  backing.put32(pic_target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = 1;
  cpu.registers[8] = 10;
  cpu.registers[9] = 20;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued &&
          run.executed == 4 && cpu.pc == base + 16);
  require(cpu.registers[8] == 11 && cpu.registers[9] == 22);
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = conditional_target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[0] = 1;
  cpu.registers[8] = 30;
  cpu.registers[9] = 40;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 5);
  require(cpu.registers[0] == 0);
  require(cpu.registers[8] == 31 && cpu.registers[9] == 42);
  require(cpu.nzcv == 0x6000'0000U);
  cpu.pc = base;
  cpu.registers[0] = 2;
  cpu.registers[8] = 50;
  cpu.registers[9] = 60;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 5);
  require(cpu.registers[0] == 1);
  require(cpu.registers[8] == 51 && cpu.registers[9] == 62);
  require(cpu.nzcv == 0x2000'0000U);

  cpu.pc = pic_source;
  cpu.registers[5] = pic_target;
  cpu.registers[8] = 70;
  cpu.registers[9] = 80;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.registers[8] == 70 && cpu.registers[9] == 80);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  require(cpu.registers[8] == 70 && cpu.registers[9] == 80);
  cpu.pc = pic_source;
  cpu.registers[2] = 0;
  cpu.registers[8] = 90;
  cpu.registers[9] = 100;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 3);
  require(cpu.registers[2] == 1);
  require(cpu.registers[8] == 90 && cpu.registers[9] == 100);
  executor.end_execution();
}

void linked_fault_keeps_precise_count_and_pc() {
  constexpr unsigned long generation = 8308;
  constexpr std::uint64_t base = 0xe4'0000;
  constexpr auto target = base + 0x40;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, branch(base + 8, target));
  backing.put32(target - base, 0xf940'0101U); // ldr x1, [x8]
  backing.put32(target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = target;
  cpu.registers[8] = 0x1234'0000;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault && run.executed == 0);

  cpu.pc = base;
  cpu.registers[8] = 0x1233'ffff;
  cpu.registers[9] = 17;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault);
  require(run.executed == 3);
  require(cpu.pc == target);
  require(cpu.registers[8] == 0x1234'0000 &&
          cpu.registers[9] == 19);
  require(executor.fault_address() == cpu.registers[8]);
  executor.end_execution();
}

void linked_illegal_and_breakpoint_publish_carried_state() {
  constexpr unsigned long generation = 8317;
  constexpr std::uint64_t base = 0xed'0000;
  constexpr auto illegal_target = base + 0x40;
  constexpr auto breakpoint_source = base + 0x80;
  constexpr auto breakpoint_target = base + 0xc0;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, branch(base + 8, illegal_target));
  backing.put32(illegal_target - base, 0); // permanently unallocated
  backing.put32(breakpoint_source - base,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(breakpoint_source - base + 4,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(breakpoint_source - base + 8,
                branch(breakpoint_source + 8, breakpoint_target));
  backing.put32(breakpoint_target - base, breakpoint);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = illegal_target;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::illegal_instruction &&
          run.executed == 0 && cpu.pc == illegal_target);
  cpu.pc = breakpoint_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::breakpoint &&
          run.executed == 1 && cpu.pc == breakpoint_target);

  cpu.pc = base;
  cpu.registers[8] = 10;
  cpu.registers[9] = 20;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::illegal_instruction &&
          run.executed == 3 && cpu.pc == illegal_target);
  require(cpu.registers[8] == 11 && cpu.registers[9] == 22);

  cpu.pc = breakpoint_source;
  cpu.registers[8] = 30;
  cpu.registers[9] = 40;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::breakpoint &&
          run.executed == 4 && cpu.pc == breakpoint_target);
  require(cpu.registers[8] == 31 && cpu.registers[9] == 42);
  executor.end_execution();
}

void publication_mode_and_unsafe_targets_remain_isolated() {
  constexpr unsigned long generation = 8309;
  constexpr std::uint64_t base = 0xe5'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto store_target = base + 0x40;
  constexpr auto terminal = base + 0x80;
  constexpr auto published_source = base + 0x100;
  constexpr auto reservation_target = base + 0x140;
  constexpr auto reservation_source = base + 0x180;
  constexpr auto fp_target = base + 0x1c0;
  constexpr auto fp_source = base + 0x200;
  constexpr auto fp_published_source = base + 0x240;
  JitBacking backing{2};
  backing.put32(0, 0x9100'0508U); // add x8, x8, #1
  backing.put32(4, 0x9100'0929U); // add x9, x9, #2
  backing.put32(8, branch(base + 8, store_target));
  backing.put32(store_target - base, 0xf900'0001U); // str x1, [x0]
  backing.put32(store_target - base + 4,
                branch(store_target + 4, terminal));
  backing.put32(terminal - base, system_call);
  backing.put32(published_source - base, 0xf900'0001U);
  backing.put32(published_source - base + 4,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(published_source - base + 8,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(published_source - base + 12,
                branch(published_source + 12, store_target));
  backing.put32(reservation_target - base, 0xc85f'7c08U); // ldxr x8,[x0]
  backing.put32(reservation_target - base + 4, system_call);
  backing.put32(reservation_source - base,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(reservation_source - base + 4,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(reservation_source - base + 8,
                branch(reservation_source + 8, reservation_target));
  backing.put32(fp_target - base, 0x9e66'0000U); // fmov x0, d0
  backing.put32(fp_target - base + 4, system_call);
  backing.put32(fp_source - base,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(fp_source - base + 4,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(fp_source - base + 8,
                branch(fp_source + 8, fp_target));
  backing.put32(fp_published_source - base,
                0x9e66'0003U); // fmov x3, d0
  backing.put32(fp_published_source - base + 4,
                0x9100'0508U); // add x8, x8, #1
  backing.put32(fp_published_source - base + 8,
                0x9100'0929U); // add x9, x9, #2
  backing.put32(fp_published_source - base + 12,
                branch(fp_published_source + 12, fp_target));
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.registers[0] = base + page;
  cpu.registers[1] = 0x1122'3344'5566'7788ULL;
  cpu.pc = terminal;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = store_target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[8] = 10;
  cpu.registers[9] = 20;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 3);
  require(cpu.pc == store_target);
  require(cpu.registers[8] == 11 && cpu.registers[9] == 22);
  cpu.pc = published_source;
  cpu.registers[8] = 30;
  cpu.registers[9] = 40;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 7);
  require(backing.get64(page) == cpu.registers[1]);
  require(cpu.registers[8] == 31 && cpu.registers[9] == 42);
  cpu.pc = reservation_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.registers[8] == cpu.registers[1] &&
          cpu.registers[9] == 42);
  executor.end_execution();
  cpu.pc = reservation_source;
  cpu.registers[8] = 50;
  cpu.registers[9] = 60;
  require(executor.begin_execution(generation, cpu.pc));
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 3);
  require(cpu.pc == reservation_target);
  require(cpu.registers[8] == 51 && cpu.registers[9] == 62);
  cpu.pc = fp_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  cpu.pc = fp_source;
  cpu.registers[8] = 70;
  cpu.registers[9] = 80;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 3);
  require(cpu.pc == fp_target);
  require(cpu.registers[8] == 71 && cpu.registers[9] == 82);
  cpu.pc = fp_published_source;
  cpu.registers[8] = 90;
  cpu.registers[9] = 100;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call &&
          run.executed == 6);
  require(cpu.registers[8] == 91 && cpu.registers[9] == 102);
  executor.end_execution();
}

} // namespace

extern "C" std::uint64_t
uml_arm64_jit_chain_abi_callback(void *opaque) {
  auto &context = *static_cast<AbiProbeContext *>(opaque);
  const auto run = run_block(*context.executor, *context.memory,
                             context.generation, *context.cpu);
  return static_cast<std::uint32_t>(run.event) |
         (static_cast<std::uint64_t>(run.executed) << 32U);
}

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  host_callee_saved_registers_are_restored();
  zero_one_two_three_and_many_links();
  fetch_window_blocks_chain();
  exact_fairness_commit_boundary();
  non_dividing_fairness_quantum_restores_the_source();
  direct_and_pic_links_carry_x8_x9();
  carried_indirect_operands_survive_miss_and_success();
  linked_helper_observes_canonical_x8_x9();
  linked_helper_writes_x8_x9_before_a_linked_consumer();
  continuing_tlb_helper_reloads_x8_x9();
  conditional_and_pic_exits_chain();
  linked_fault_keeps_precise_count_and_pc();
  linked_illegal_and_breakpoint_publish_carried_state();
  publication_mode_and_unsafe_targets_remain_isolated();
  return 0;
}
