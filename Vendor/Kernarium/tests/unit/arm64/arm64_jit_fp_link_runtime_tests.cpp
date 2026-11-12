// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t vector_add =
    0x4ea1'8400U; // add v0.4s, v0.4s, v1.4s
constexpr std::uint32_t native_add =
    0x1e61'2800U; // fadd d0, d0, d1
constexpr std::uint32_t native_divide =
    0x1e22'1820U; // fdiv s0, s1, s2
constexpr std::uint32_t native_round_current =
    0x1e27'c083U; // frinti s3, s4
constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1U;

[[nodiscard]] std::uint64_t native_fpcr() {
  std::uint64_t result{};
  __asm__ volatile("mrs %0, fpcr" : "=r"(result) : : "memory");
  return result;
}

[[nodiscard]] std::uint64_t native_fpsr() {
  std::uint64_t result{};
  __asm__ volatile("mrs %0, fpsr" : "=r"(result) : : "memory");
  return result;
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x1400'0000U |
         (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
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

void set_vector_lanes(arm64::VectorRegister &reg, std::uint32_t value) {
  const std::array lanes{value, value, value, value};
  std::memcpy(reg.bytes.data(), lanes.data(), sizeof(lanes));
}

[[nodiscard]] std::uint32_t
first_vector_lane(const arm64::VectorRegister &reg) {
  std::uint32_t result{};
  std::memcpy(&result, reg.bytes.data(), sizeof(result));
  return result;
}

void deferred_vector_self_loop_obeys_fairness() {
  constexpr unsigned long generation = 8401;
  constexpr std::uint64_t base = 0xec'0000;
  JitBacking backing{1};
  backing.put32(0, vector_add);
  backing.put32(4, 0x9100'0400U); // add x0, x0, #1
  backing.put32(8, 0x9100'0442U); // add x2, x2, #1
  backing.put32(12, branch(base + 12, base));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  std::array<arm64::VectorRegister, 32> deferred{};
  set_vector_lanes(deferred[1], 1);
  arm64::CpuState cpu{};
  cpu.defer_fp_load(deferred.data(), 0, 0, arm64::fpcr_writable_mask);
  cpu.pc = base;

  require(executor.begin_execution(generation, cpu.pc));
  const auto run = run_block(executor, memory, generation, cpu);
  const auto limit =
      uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == limit);
  require(cpu.pc == base);
  require(cpu.registers[0] == limit / 4);
  require(cpu.registers[2] == limit / 4);
  require(first_vector_lane(cpu.vectors[0]) == limit / 4);
  require(cpu.fp_loaded && cpu.fp_dirty);
  executor.end_execution();
}

void direct_capability_requirements_are_exact() {
  constexpr unsigned long generation = 8402;
  constexpr std::uint64_t base = 0xf0'0000;
  constexpr auto vector_target = base + 0x100;
  constexpr auto native_target = base + 0x200;
  constexpr auto plain_source = base + 0x300;
  constexpr auto state_source = base + 0x400;
  constexpr auto state_to_native = base + 0x500;
  constexpr auto native_source = base + 0x600;
  JitBacking backing{1};
  backing.put32(vector_target - base, vector_add);
  backing.put32(vector_target - base + 4, system_call);
  backing.put32(native_target - base, native_add);
  backing.put32(native_target - base + 4, system_call);
  backing.put32(plain_source - base,
                branch(plain_source, vector_target));
  backing.put32(state_source - base, vector_add);
  backing.put32(state_source - base + 4,
                branch(state_source + 4, vector_target));
  backing.put32(state_to_native - base, vector_add);
  backing.put32(state_to_native - base + 4,
                branch(state_to_native + 4, native_target));
  backing.put32(native_source - base, native_add);
  backing.put32(native_source - base + 4,
                branch(native_source + 4, native_target));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 1);
  cpu.pc = vector_target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = native_target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  arm64::restore_native_fp_environment(cpu);

  cpu.pc = plain_source;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == vector_target);

  cpu.pc = state_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);

  cpu.pc = state_to_native;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  require(cpu.pc == native_target);

  cpu.pc = native_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);
  require(cpu.gadget_fp_active);
  arm64::restore_native_fp_environment(cpu);
  executor.end_execution();
}

void control_and_status_state_survives_links() {
  constexpr unsigned long generation = 8403;
  constexpr std::uint64_t base = 0xf4'0000;
  constexpr auto status_target = base + 0x100;
  constexpr auto status_source = base + 0x200;
  constexpr auto trap_source = base + 0x300;
  constexpr auto trap_target = base + 0x380;
  JitBacking backing{1};
  backing.put32(status_target - base,
                0xd53b'4424U); // mrs x4, fpsr
  backing.put32(status_target - base + 4, system_call);
  backing.put32(status_source - base,
                0xd51b'4423U); // msr fpsr, x3
  backing.put32(status_source - base + 4,
                branch(status_source + 4, status_target));
  backing.put32(trap_source - base,
                0xd51b'4405U); // msr fpcr, x5
  backing.put32(trap_source - base + 4,
                branch(trap_source + 4, trap_target));
  backing.put32(trap_target - base, 0xd280'00c6U); // mov x6, #6
  backing.put32(trap_target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = status_target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  arm64::restore_native_fp_environment(cpu);

  constexpr std::uint32_t expected_status = 0x0800'0011U;
  cpu.fpsr = 0;
  cpu.registers[3] = expected_status;
  cpu.pc = status_source;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);
  require(cpu.registers[4] == expected_status);
  require(cpu.fpsr == expected_status);

  cpu.fpcr = 0;
  cpu.registers[5] = 1U << 8U;
  cpu.registers[6] = 0;
  cpu.pc = trap_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == trap_source + 4);
  require(cpu.registers[6] == 0);
  require((cpu.fpcr & (1U << 8U)) != 0);
  arm64::restore_native_fp_environment(cpu);
  executor.end_execution();
}

void native_status_is_deferred_until_an_architectural_observer() {
  constexpr unsigned long generation = 8407;
  constexpr std::uint64_t base = 0x104'0000;
  constexpr auto observer = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, native_divide);
  backing.put32(4, system_call);
  backing.put32(observer - base, 0xd53b'4423U); // mrs x3, fpsr
  backing.put32(observer - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 0x3f80'0000U);
  set_vector_lanes(cpu.vectors[2], 0);
  cpu.pc = base;
  const auto host_fpcr = native_fpcr();
  const auto host_fpsr = native_fpsr();

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  require(cpu.gadget_fp_active);
  require((cpu.fpsr & fpsr_divide_by_zero) == 0);

  cpu.pc = observer;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  require((cpu.registers[3] & fpsr_divide_by_zero) != 0);
  require(cpu.gadget_fp_active);
  require((cpu.fpsr & fpsr_divide_by_zero) == 0);

  executor.end_execution();
  require(!cpu.gadget_fp_active);
  require((cpu.fpsr & fpsr_divide_by_zero) != 0);
  require(native_fpcr() == host_fpcr);
  require(native_fpsr() == host_fpsr);
}

void semantic_helper_materializes_status_and_restores_host() {
  constexpr unsigned long generation = 8408;
  constexpr std::uint64_t base = 0x108'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint64_t value = 0x1122'3344'5566'7788ULL;
  JitBacking backing{2};
  backing.put32(0, native_divide);
  backing.put32(4, 0xc89f'fc83U); // stlr x3, [x4]
  backing.put32(8, system_call);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 0x3f80'0000U);
  set_vector_lanes(cpu.vectors[2], 0);
  cpu.registers[3] = value;
  cpu.registers[4] = base + page;
  cpu.pc = base;
  const auto host_fpcr = native_fpcr();
  const auto host_fpsr = native_fpsr();

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == base + 4);
  require(cpu.gadget_fp_active);
  require((cpu.fpsr & fpsr_divide_by_zero) == 0);

  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == base + 8);
  require(backing.get64(page) == value);
  require(!cpu.gadget_fp_active);
  require((cpu.fpsr & fpsr_divide_by_zero) != 0);
  require(native_fpcr() == host_fpcr);
  require(native_fpsr() == host_fpsr);

  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 1);
  executor.end_execution();
}

void changing_cpu_materializes_the_previous_owner() {
  constexpr unsigned long generation = 8409;
  constexpr std::uint64_t base = 0x10c'0000;
  constexpr auto second_pc = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, native_divide);
  backing.put32(4, system_call);
  backing.put32(second_pc - base, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState first{};
  set_vector_lanes(first.vectors[1], 0x3f80'0000U);
  set_vector_lanes(first.vectors[2], 0);
  first.pc = base;
  arm64::CpuState second{};
  second.pc = second_pc;
  const auto host_fpcr = native_fpcr();
  const auto host_fpsr = native_fpsr();

  require(executor.begin_execution(generation, first.pc));
  auto run = run_block(executor, memory, generation, first);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  require(first.gadget_fp_active);
  require((first.fpsr & fpsr_divide_by_zero) == 0);

  run = run_block(executor, memory, generation, second);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 1);
  require(!first.gadget_fp_active);
  require((first.fpsr & fpsr_divide_by_zero) != 0);
  require(native_fpcr() == host_fpcr);
  require(native_fpsr() == host_fpsr);
  executor.end_execution();
}

void returning_miss_preserves_native_control_and_status() {
  constexpr unsigned long generation = 8411;
  constexpr std::uint64_t base = 0x114'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint64_t value = 0x0123'4567'89ab'cdefULL;
  JitBacking backing{2};
  backing.put32(0, 0xd51b'4405U);  // msr fpcr, x5
  backing.put32(4, native_divide);
  backing.put32(8, 0xf940'00e3U);  // ldr x3, [x7]
  backing.put32(12, native_round_current);
  backing.put32(16, 0xd53b'4426U); // mrs x6, fpsr
  backing.put32(20, system_call);
  backing.put64(page, value);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 0x3f80'0000U);
  set_vector_lanes(cpu.vectors[2], 0);
  set_vector_lanes(cpu.vectors[4], 0x3fa0'0000U);
  cpu.registers[5] = 1U << 22U; // Round toward positive infinity.
  cpu.registers[7] = base + page;
  cpu.pc = base;
  const auto host_fpcr = native_fpcr();
  const auto host_fpsr = native_fpsr();

  require(executor.begin_execution(generation, cpu.pc));
  const auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 6);
  require(cpu.registers[3] == value);
  require(first_vector_lane(cpu.vectors[3]) == 0x4000'0000U);
  require((cpu.registers[6] & fpsr_divide_by_zero) != 0);
  require((cpu.fpcr & (3U << 22U)) == (1U << 22U));
  executor.end_execution();
  require((cpu.fpsr & fpsr_divide_by_zero) != 0);
  require(native_fpcr() == host_fpcr);
  require(native_fpsr() == host_fpsr);
}

void stateful_store_links_require_publication() {
  constexpr unsigned long generation = 8405;
  constexpr std::uint64_t base = 0xfc'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto target = base + 0x100;
  constexpr auto read_only_source = base + 0x200;
  constexpr auto published_source = base + 0x300;
  JitBacking backing{2};
  backing.put32(target - base, 0x3d80'0020U); // str q0, [x1]
  backing.put32(target - base + 4, system_call);
  backing.put32(read_only_source - base, vector_add);
  backing.put32(read_only_source - base + 4,
                branch(read_only_source + 4, target));
  backing.put32(published_source - base,
                0xf900'0062U); // str x2, [x3]
  backing.put32(published_source - base + 4, vector_add);
  backing.put32(published_source - base + 8,
                branch(published_source + 8, target));
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 1);
  cpu.registers[1] = base + page + 32;
  cpu.registers[2] = 0x1122'3344'5566'7788ULL;
  cpu.registers[3] = base + page;
  cpu.pc = target;

  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = read_only_source;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  require(cpu.pc == target);

  cpu.pc = published_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 5);
  require(backing.get64(page) == cpu.registers[2]);
  executor.end_execution();
}

void linked_successor_observes_dirty_constrained_binding() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr unsigned long generation = 8414;
  constexpr std::uint64_t base = 0x120'0000;
  constexpr auto target = base + 0x100;
  constexpr auto source = base + 0x200;
  constexpr std::uint32_t indexed_halfword_multiply =
      0x4f40'8000U; // mul v0.8h, v0.8h, v0.h[0]
  constexpr std::uint32_t copy_result =
      0x4ea0'1c01U; // mov v1.16b, v0.16b
  constexpr std::uint32_t input_lanes = 0x0202'0202U;
  constexpr std::uint32_t result_lanes = 0x0804'0804U;

  JitBacking backing{1};
  backing.put32(target - base, copy_result);
  backing.put32(target - base + 4U, system_call);
  backing.put32(source - base, indexed_halfword_multiply);
  backing.put32(source - base + 4U,
                branch(source + 4U, target));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[0], input_lanes);
  cpu.pc = source;

  /*
   * Compile the source before its target. The first edge returns through C++
   * after flushing dirty q6; compiling the target then proves it imports the
   * published architectural value.
   */
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2U);
  require(cpu.pc == target);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2U);
  require(first_vector_lane(cpu.vectors[0]) == result_lanes);
  require(first_vector_lane(cpu.vectors[1]) == result_lanes);
  executor.end_execution();

  /*
   * The warmed edge now enters the target's internal entry directly. The
   * source's dirty low-bank value must still be flushed before BR, and the
   * successor must observe it without relying on the outer C++ boundary.
   */
  set_vector_lanes(cpu.vectors[0], input_lanes);
  set_vector_lanes(cpu.vectors[1], 0U);
  cpu.pc = source;
  require(executor.begin_execution(generation, cpu.pc));
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4U);
  require(first_vector_lane(cpu.vectors[0]) == result_lanes);
  require(first_vector_lane(cpu.vectors[1]) == result_lanes);
  executor.end_execution();
}

void linked_vector_fault_is_precise_for_page_size() {
  constexpr unsigned long generation = 8406;
  constexpr std::uint64_t base = 0x100'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto target = base + 0x100;
  constexpr auto source = base + 0x200;
  JitBacking backing{1};
  backing.put32(target - base, 0x3dc0'0000U); // ldr q0, [x0]
  backing.put32(target - base + 4, system_call);
  backing.put32(source - base, vector_add);
  backing.put32(source - base + 4, branch(source + 4, target));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.registers[0] = base + page;
  cpu.pc = target;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault && run.executed == 0);

  cpu.pc = source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault && run.executed == 2);
  require(cpu.pc == target);
  require(executor.fault_address() == cpu.registers[0]);
  require(cpu.fp_loaded && cpu.fp_dirty);
  executor.end_execution();
}

void trap_mode_runs_a_deferred_atomic() {
  /* A native atomic leaves its sequence for the exclusive helper when its
   * address is misaligned, and FEAT_LSE2 lets that helper complete an access
   * within one 16-byte block. */
  if (!::uml::platform::apple::probed_host_capabilities().has(
          ::uml::cpu::ArmFeature::lse2))
    return;
  constexpr unsigned long generation = 8415;
  constexpr std::uint64_t base = 0x124'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint32_t load_add = 0xf821'0002U; // ldadd x1, x2, [x0]
  JitBacking backing{2};
  backing.put32(0, load_add);
  backing.put32(4, load_add);
  backing.put64(page, 40);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  /*
   * An enabled trap runs each instruction as its own translation. A
   * misaligned address sends the native atomic to its exclusive helper,
   * which a block with a direct store defers until its store publication
   * has ended.
   */
  cpu.fpcr = 1U << 8U;
  cpu.registers[0] = base + page + 1;
  cpu.registers[1] = 2;
  cpu.pc = base;

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == base + 4);
  /* The eight bytes from the second byte were zero. */
  require(cpu.registers[2] == 0);
  require(backing.get64(page) == 40 + (2U << 8U));

  /* Across a 16-byte block, so the alignment fault stands under LSE2. */
  cpu.registers[0] = base + page + 9;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault && run.executed == 0);
  require(cpu.pc == base + 4);
  require(executor.fault_address() == base + page + 9);
  require(backing.get64(page) == 40 + (2U << 8U));
  executor.end_execution();
}

void ordered_loads_stay_native() {
  /* LDAR and LDAPR run as host load-acquires inside the block: the block
   * retires whole in one dispatch with the native FP environment still
   * active, and a load from an unmapped page faults precisely. */
  constexpr unsigned long generation = 8412;
  constexpr std::uint64_t base = 0x118'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr std::uint64_t value = 0x8877'6655'4433'2211ULL;
  JitBacking backing{2};
  backing.put32(0, native_divide);
  backing.put32(4, 0x08df'fc85U); // ldarb w5, [x4]
  backing.put32(8, 0xc8df'fc83U); // ldar x3, [x4]
  const bool lrcpc = ::uml::platform::apple::probed_host_capabilities().has(
      ::uml::cpu::ArmFeature::lrcpc);
  // ldaprh w6, [x4], or a second ldar when the host lacks FEAT_LRCPC.
  backing.put32(12, lrcpc ? 0x78bf'c086U : 0x48df'fc86U);
  backing.put32(16, 0xc8df'fce7U); // ldar x7, [x7]
  backing.put32(20, system_call);
  backing.put64(page, value);
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  set_vector_lanes(cpu.vectors[1], 0x3f80'0000U);
  set_vector_lanes(cpu.vectors[2], 0);
  cpu.registers[4] = base + page;
  cpu.registers[7] = base + page;
  cpu.pc = base;
  const auto host_fpcr = native_fpcr();
  const auto host_fpsr = native_fpsr();

  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == Event::system_call && run.executed == 6);
  require(cpu.registers[5] == (value & 0xffU) && cpu.registers[3] == value &&
          cpu.registers[6] == (value & 0xffffU) && cpu.registers[7] == value);
  require(cpu.gadget_fp_active);
  executor.end_execution();
  require(!cpu.gadget_fp_active);
  require(native_fpcr() == host_fpcr && native_fpsr() == host_fpsr);

  cpu.pc = base + 4;
  cpu.registers[4] = base + 2U * page;
  cpu.registers[5] = 0;
  require(executor.begin_execution(generation, cpu.pc));
  run = run_block(executor, memory, generation, cpu);
  executor.end_execution();
  require(run.event == Event::memory_fault && run.executed == 0);
  require(cpu.pc == base + 4 && cpu.registers[5] == 0);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  deferred_vector_self_loop_obeys_fairness();
  direct_capability_requirements_are_exact();
  control_and_status_state_survives_links();
  native_status_is_deferred_until_an_architectural_observer();
  semantic_helper_materializes_status_and_restores_host();
  changing_cpu_materializes_the_previous_owner();
  returning_miss_preserves_native_control_and_status();
  stateful_store_links_require_publication();
  linked_successor_observes_dirty_constrained_binding();
  linked_vector_fault_is_precise_for_page_size();
  trap_mode_runs_a_deferred_atomic();
  ordered_loads_stay_native();
  return 0;
}
