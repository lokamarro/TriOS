// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 exclusives on the JIT: the fused LL/SC runs (a load-exclusive,
 * register arithmetic and a store-exclusive become one host pair), the same
 * instructions through the slow helper, and the monitor rules between them.
 * The compare-and-swap idiom, fault and alignment exits, two threads counting
 * exactly and an ordinary store beating a reservation are covered too. The
 * fused loop must make no helper call, also with a compare and conditional
 * branch or a CBNZ between the load and the store (A32 and T32). */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <pthread.h>
#include <thread>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

/* A thread with the stack of a vCPU: the executor keeps its fetch and
 * translation buffers on the stack. */
class BigThread final {
public:
  explicit BigThread(std::function<void()> body) : body_{std::move(body)} {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8U << 20);
    require(pthread_create(&thread_, &attributes, &BigThread::entry, this) == 0);
    pthread_attr_destroy(&attributes);
  }
  void join() { require(pthread_join(thread_, nullptr) == 0); }

private:
  static void *entry(void *self) {
    static_cast<BigThread *>(self)->body_();
    return nullptr;
  }
  std::function<void()> body_;
  pthread_t thread_{};
};

/* B<cond> from the word at `from` to the word at `to`. */
constexpr std::uint32_t arm_branch(unsigned cond, int from, int to) {
  return cond << 28 | 0x0a00'0000U |
         (static_cast<std::uint32_t>(to - from - 2) & 0x00ff'ffffU);
}
/* B<cond>.N from the halfword at `from` to the halfword at `to`. */
constexpr std::uint16_t thumb_branch(unsigned cond, int from, int to) {
  return static_cast<std::uint16_t>(
      0xd000U | cond << 8 | (static_cast<std::uint32_t>(to - from - 2) & 0xffU));
}

constexpr unsigned ne = 1, eq = 0;
constexpr std::uint32_t arm_svc = 0xef00'0000U;
constexpr std::uint32_t arm_ldrex_r2 = 0xe190'2f9fU;   // ldrex r2, [r0]
constexpr std::uint32_t arm_add_r2_1 = 0xe282'2001U;   // add r2, r2, #1
constexpr std::uint32_t arm_strex_r3_r2 = 0xe180'3f92U; // strex r3, r2, [r0]
constexpr std::uint32_t arm_cmp_r3_0 = 0xe353'0000U;   // cmp r3, #0

/* The shape of a fused run: the number of guest instructions the first block
 * holds. */
std::size_t block_instructions(Fixture &fixture, std::size_t offset,
                               std::size_t words, bool thumb) {
  std::vector<std::byte> bytes(words * (thumb ? 2 : 4));
  fixture.read_code(offset, bytes);
  const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
            : key::a32_key(code_base + static_cast<std::uint32_t>(offset)),
      bytes);
  return compiled.code.guest_instructions;
}

void atomic_add_loop_fuses_and_counts(unsigned long generation) {
  Fixture fixture{generation};
  // r4 iterations of: ldrex/add/strex/cmp/bne, then the counter.
  fixture.code32(0, {0xe3a0'4ffaU, // mov r4, #1000
                     arm_ldrex_r2, arm_add_r2_1, arm_strex_r3_r2, arm_cmp_r3_0,
                     arm_branch(ne, 5, 1), // bne to the ldrex
                     0xe254'4001U,         // subs r4, r4, #1
                     arm_branch(ne, 7, 1), arm_svc});
  fixture.data32(0, 5);
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base;
  ::uml::cpu::arm64::aarch32::reset_a32_library_exits();
  ::uml::cpu::arm64::aarch32::count_a32_library_exits(true);
  const auto result = fixture.run(cpu, 1u << 20);
  ::uml::cpu::arm64::aarch32::count_a32_library_exits(false);
  require(result.reason == ExitReason::system_call);
  // The fused pair never reaches the helper.
  require(::uml::cpu::arm64::aarch32::a32_library_exit_total() == 0);
  require(fixture.read32(0) == 1005);
  require(cpu.registers[2] == 1005);
  require(cpu.registers[3] == 0);
  require(cpu.registers[4] == 0);
  // From the load-exclusive: the whole run, the compare and the branch.
  require(block_instructions(fixture, 4, 5, false) == 5);
}

void conditional_store_compare_and_swap(unsigned long generation) {
  Fixture fixture{generation};
  // 1: ldrex r3,[r0]; cmp r3,r1; strexeq r12,r2,[r0]; cmpeq r12,#1; beq 1b
  fixture.code32(0, {0xe190'3f9fU, 0xe153'0001U, 0x0180'cf92U, 0x035c'0001U,
                     arm_branch(eq, 4, 0), arm_svc});
  fixture.data32(0, 77);
  for (const bool match : {true, false}) {
    auto cpu = Fixture::task(false);
    cpu.registers[0] = data_base;
    cpu.registers[1] = match ? 77 : 78; // the value the guest expects
    cpu.registers[2] = 99;
    cpu.registers[12] = 0xdead;
    fixture.data32(0, 77);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    require(cpu.registers[3] == 77);
    if (match) {
      require(fixture.read32(0) == 99);
      require(cpu.registers[12] == 0);
    } else {
      require(fixture.read32(0) == 77);
      require(cpu.registers[12] == 0xdead); // the store did not execute
    }
  }
  require(block_instructions(fixture, 0, 5, false) == 5);
}

void thumb_fused_add(unsigned long generation) {
  Fixture fixture{generation};
  // ldrex r2,[r0]; add r2,r1; strex r3,r2,[r0]; cmp r3,#0; bne.n 1b; svc
  fixture.code16(0, {0xe850, 0x2f00, 0x440a, 0xe840, 0x2300, 0x2b00,
                     thumb_branch(ne, 6, 0), 0xdf00});
  fixture.data32(0, 40);
  auto cpu = Fixture::task(true);
  cpu.registers[0] = data_base;
  cpu.registers[1] = 2;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(fixture.read32(0) == 42);
  require(cpu.registers[2] == 42 && cpu.registers[3] == 0);
  require(block_instructions(fixture, 0, 8, true) == 5);
}

/* A branch out of the run: LDREX, a compare or CBNZ, a conditional branch to
 * the exit, then the STREX. The run is one fused block with no helper exit
 * on either arm, and the taken arm closes the monitor. */
void branch_out_of_the_run(unsigned long generation) {
  const auto arm = [&](Fixture &fixture, unsigned mode) {
    // mode 0: ldrex r3; cmp r3,r1; bne out; strex r12,r2; cmp r12,#0; bne 0.
    // mode 1: ldr r6,[r0]; cmp r5,#0 in the previous block; ldrex r2;
    //         bne out; strex r3,r1; cmp r3,#0; bne the compare.
    // mode 2: cmp r5,#0; ldrex r2; strexeq r3,r1; cmpeq r3,#1; beq the
    // compare. A first store to a page may fail, as any may.
    if (mode == 2)
      fixture.code32(0, {0xe590'6000U, 0xe355'0000U, 0xe190'2f9fU,
                         0x0180'3f91U, 0x0353'0001U, arm_branch(eq, 5, 1),
                         arm_svc});
    else if (mode == 0)
      fixture.code32(0, {0xe190'3f9fU, 0xe153'0001U, arm_branch(ne, 2, 6),
                         0xe180'cf92U, 0xe35c'0000U, arm_branch(ne, 5, 0),
                         arm_svc});
    else
      fixture.code32(0, {0xe590'6000U, 0xe355'0000U, 0xe190'2f9fU,
                         arm_branch(ne, 3, 7), 0xe180'3f91U, arm_cmp_r3_0,
                         arm_branch(ne, 6, 1), arm_svc});
  };
  for (const unsigned mode : {0U, 1U, 2U}) {
    for (const bool stays : {false, true}) {
      Fixture fixture{generation + mode};
      arm(fixture, mode);
      fixture.data32(0, 77);
      auto cpu = Fixture::task(false);
      cpu.registers[0] = data_base;
      cpu.registers[1] = mode == 0 ? (stays ? 78 : 77) : 99;
      cpu.registers[2] = 99;
      cpu.registers[5] = stays ? 1 : 0;
      cpu.registers[12] = 0xdead;
      cpu.registers[3] = 0xbeef;
      cpu.exclusive_size = 4;
      ::uml::cpu::arm64::aarch32::reset_a32_library_exits();
      ::uml::cpu::arm64::aarch32::count_a32_library_exits(true);
      const auto result = fixture.run(cpu);
      ::uml::cpu::arm64::aarch32::count_a32_library_exits(false);
      require(result.reason == ExitReason::system_call);
      require(::uml::cpu::arm64::aarch32::a32_library_exit_total() == 0);
      require(cpu.exclusive_size == 0);
      if (mode == 0) {
        require(cpu.registers[3] == 77);
        require(fixture.read32(0) == (stays ? 77U : 99U));
        require(cpu.registers[12] == (stays ? 0xdeadU : 0U));
      } else {
        require(cpu.registers[2] == 77);
        require(fixture.read32(0) == (stays ? 77U : 99U));
        require(cpu.registers[3] == (stays ? 0xbeefU : 0U));
      }
    }
  }
  Fixture fixture{generation + 3};
  arm(fixture, 0);
  require(block_instructions(fixture, 0, 7, false) == 6);
}

void thumb_branch_out_of_the_run(unsigned long generation) {
  // 1: ldrex r2,[r0]; cmp r2,r1; bne.n 2f; strex r3,r4,[r0]; cmp r3,#0;
  // bne.n 1b; 2: svc
  // 1: ldrex r2,[r0]; cbnz r2,2f; strex r3,r1,[r0]; cmp r3,#0; bne.n 1b;
  // 2: svc
  for (const unsigned form : {0U, 1U}) {
    for (const bool stays : {false, true}) {
      Fixture fixture{generation + form};
      if (form == 0)
        fixture.code16(0, {0xe850, 0x2f00, 0x428a, thumb_branch(ne, 3, 8),
                           0xe840, 0x4300, 0x2b00, thumb_branch(ne, 7, 0),
                           0xdf00});
      else
        fixture.code16(0, {0xe850, 0x2f00, 0xb91a, 0xe840, 0x1300, 0x2b00,
                           thumb_branch(ne, 6, 0), 0xdf00});
      fixture.data32(0, form == 0 ? 55 : (stays ? 3 : 0));
      auto cpu = Fixture::task(true);
      cpu.registers[0] = data_base;
      cpu.registers[1] = form == 0 ? (stays ? 56 : 55) : 21;
      cpu.registers[4] = 21;
      cpu.registers[3] = 0xbeef;
      cpu.exclusive_size = 4;
      ::uml::cpu::arm64::aarch32::reset_a32_library_exits();
      ::uml::cpu::arm64::aarch32::count_a32_library_exits(true);
      const auto result = fixture.run(cpu);
      ::uml::cpu::arm64::aarch32::count_a32_library_exits(false);
      require(result.reason == ExitReason::system_call);
      require(::uml::cpu::arm64::aarch32::a32_library_exit_total() == 0);
      require(cpu.exclusive_size == 0);
      require(cpu.registers[2] == (form == 0 ? 55U : (stays ? 3U : 0U)));
      if (stays) {
        require(fixture.read32(0) == (form == 0 ? 55U : 3U));
        require(cpu.registers[3] == 0xbeefU);
      } else {
        require(fixture.read32(0) == 21U);
        require(cpu.registers[3] == 0U);
      }
    }
  }
  Fixture fixture{generation + 2};
  fixture.code16(0, {0xe850, 0x2f00, 0x428a, thumb_branch(ne, 3, 8), 0xe840,
                     0x4300, 0x2b00, thumb_branch(ne, 7, 0), 0xdf00});
  require(block_instructions(fixture, 0, 9, true) == 6);
}

void byte_and_halfword_runs(unsigned long generation) {
  Fixture fixture{generation};
  // 1: ldrexb r2,[r0]; add r2,r2,#1; strexb r3,r2,[r0]; cmp r3,#0; bne 1b
  // 2: ldrexh r5,[r0]; add r5,r5,#1; strexh r6,r5,[r0]; cmp r6,#0; bne 2b
  fixture.code32(0, {0xe1d0'2f9fU, arm_add_r2_1, 0xe1c0'3f92U, arm_cmp_r3_0,
                     arm_branch(ne, 4, 0),
                     0xe1f0'5f9fU, 0xe285'5001U, 0xe1e0'6f95U, 0xe356'0000U,
                     arm_branch(ne, 9, 5), arm_svc});
  fixture.data32(0, 0x1234'56ffU);
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  // The byte wraps to 0 (only the low byte is stored back); the halfword
  // 0x5600 + 1 follows it.
  require(cpu.registers[3] == 0 && cpu.registers[6] == 0);
  require(fixture.read32(0) == 0x1234'5601U);
  // Each run is one block.
  require(block_instructions(fixture, 0, 5, false) == 5);
  require(block_instructions(fixture, 20, 5, false) == 5);
}

void monitor_rules(unsigned long generation) {
  Fixture fixture{generation};
  fixture.data32(0, 10);
  const auto run = [&](std::initializer_list<std::uint32_t> program) {
    fixture.code32(0, program);
    auto cpu = Fixture::task(false);
    cpu.registers[0] = data_base;
    cpu.registers[2] = 99;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    return cpu;
  };
  // A store between the load and the store takes the reservation away.
  fixture.data32(0, 10);
  auto cpu = run({0xe190'1f9fU,  // ldrex r1,[r0]
                  0xe580'2000U,  // str r2,[r0]
                  0xe180'3f91U,  // strex r3,r1,[r0]
                  arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 99);
  // CLREX does too.
  fixture.data32(0, 10);
  cpu = run({0xe190'1f9fU, 0xf57f'f01fU, 0xe180'3f91U, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 10);
  // A STREX with no reservation fails.
  fixture.data32(0, 10);
  cpu = run({0xe180'3f92U, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 10);
  // A STREX to another address than the load's fails.
  fixture.data32(0, 10);
  fixture.data32(8, 20);
  cpu = run({0xe190'1f9fU, 0xe280'4008U, 0xe184'3f92U, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(8) == 20);
  // The pair succeeds without anything between. It is the host's own load
  // and store exclusive, and the host store fails when anything clears the
  // host monitor in between, such as an interrupt or a context switch. The
  // architecture allows that failure and a guest retries, so the pair gets a
  // few attempts.
  bool stored = false;
  for (int attempt = 0; attempt < 16 && !stored; ++attempt) {
    fixture.data32(0, 10);
    cpu = run({0xe190'1f9fU, 0xe180'3f92U, arm_svc});
    stored = cpu.registers[3] == 0;
  }
  require(stored && fixture.read32(0) == 99);
}

void faults_and_alignment(unsigned long generation) {
  Fixture fixture{generation};
  fixture.code32(0, {arm_ldrex_r2, arm_add_r2_1, arm_strex_r3_r2, arm_svc});
  // A misaligned address is an alignment fault at the load.
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base + 2;
  cpu.registers[2] = 1234;
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::memory_fault);
  require(result.fault.alignment);
  require(result.fault.address == data_base + 2);
  require(result.retired_instructions == 0);
  require(cpu.registers[2] == 1234);
  // An unmapped address is a read fault at the load.
  cpu = Fixture::task(false);
  cpu.registers[0] = 0x6000'0000U;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::memory_fault);
  require(!result.fault.alignment);
  require(result.fault.address == 0x6000'0000U);
  require(result.retired_instructions == 0);
}

/* Two threads add through the kernel's cmpxchg helper (arm64 Linux's
 * kuser32.S, word for word, with its store-release exclusive), which does not
 * fuse: the software reservations and the tracked stores of the slow path
 * keep the count exact. */
void two_threads_through_kuser_cmpxchg(unsigned long generation) {
  constexpr std::uint32_t kuser_page = 0xffff'0000U;
  constexpr std::size_t helper = 0xfc0;
  Fixture fixture{generation};
  fixture.map_code_alias(kuser_page);
  fixture.code32(helper, {0xe192'3f9fU, 0xe053'3000U, 0x0182'3e91U,
                          0x0333'0001U, 0x0aff'fffaU, 0xf57f'f05bU,
                          0xe273'0000U, 0xe12f'ff1eU});
  // old = [r2]; do { new = old + 1 } while (cmpxchg(old, new, r2) != 0)
  fixture.code32(0, {0xe592'0000U,   // ldr r0, [r2]
                     0xe280'1001U,   // add r1, r0, #1
                     0xe300'cfc0U,   // movw r12, #0x0fc0
                     0xe34f'cfffU,   // movt r12, #0xffff
                     0xe12f'ff3cU,   // blx r12
                     0xe350'0000U,   // cmp r0, #0
                     arm_branch(ne, 6, 0), 0xe254'4001U,
                     arm_branch(ne, 8, 0), arm_svc});
  fixture.data32(0, 0);
  constexpr unsigned iterations = 5000;
  std::atomic<bool> go{false};
  std::atomic<unsigned> done{0};
  const auto worker = [&] {
    const auto engine = Fixture::new_engine();
    auto cpu = Fixture::task(false);
    cpu.registers[2] = data_base;
    cpu.registers[4] = iterations;
    while (!go.load())
      std::this_thread::yield();
    for (;;) {
      const auto result = fixture.run_on(*engine, cpu, 1u << 20);
      if (result.reason == ExitReason::system_call)
        break;
      require(result.reason == ExitReason::instruction_budget ||
              result.reason == ExitReason::interrupt);
    }
    ++done;
  };
  BigThread first{worker};
  BigThread second{worker};
  go = true;
  first.join();
  second.join();
  require(done == 2);
  require(fixture.read32(0) == 2 * iterations);
}

void two_threads_count_exactly(unsigned long generation) {
  Fixture fixture{generation};
  // r4 iterations of an atomic increment of [r0], then svc.
  fixture.code32(0, {arm_ldrex_r2, arm_add_r2_1, arm_strex_r3_r2, arm_cmp_r3_0,
                     arm_branch(ne, 4, 0), 0xe254'4001U, arm_branch(ne, 6, 0),
                     arm_svc});
  fixture.data32(0, 0);
  constexpr unsigned iterations = 20000;
  std::atomic<bool> go{false};
  std::atomic<unsigned> done{0};
  const auto worker = [&] {
    const auto engine = Fixture::new_engine();
    auto cpu = Fixture::task(false);
    cpu.registers[0] = data_base;
    cpu.registers[4] = iterations;
    while (!go.load())
      std::this_thread::yield();
    // The budget bounds each call; keep running until the loop ends.
    for (;;) {
      const auto result = fixture.run_on(*engine, cpu, 1u << 20);
      if (result.reason == ExitReason::system_call)
        break;
      require(result.reason == ExitReason::instruction_budget ||
              result.reason == ExitReason::interrupt);
    }
    ++done;
  };
  BigThread first{worker};
  BigThread second{worker};
  go = true;
  first.join();
  second.join();
  require(done == 2);
  require(fixture.read32(0) == 2 * iterations);
}

void ordinary_store_beats_the_reservation(unsigned long generation) {
  Fixture fixture{generation};
  // One thread increments with LL/SC; the other adds 1000 with a plain
  // load-add-store loop that runs concurrently. LL/SC increments are never
  // lost to an ordinary store that lands inside its window, but the plain
  // thread's own read-modify-write is not atomic, so only the LL/SC thread's
  // contribution is checked: the counter in its own word.
  fixture.code32(0, {arm_ldrex_r2, arm_add_r2_1, arm_strex_r3_r2, arm_cmp_r3_0,
                     arm_branch(ne, 4, 0), 0xe254'4001U, arm_branch(ne, 6, 0),
                     arm_svc});
  // Plain thread: str to a neighbouring word of the same line, repeatedly.
  fixture.code32(64, {0xe580'1004U, 0xe254'4001U, arm_branch(ne, 18, 16),
                      arm_svc});
  fixture.data32(0, 0);
  std::atomic<bool> go{false};
  const auto atomic_worker = [&] {
    const auto engine = Fixture::new_engine();
    auto cpu = Fixture::task(false);
    cpu.registers[0] = data_base;
    cpu.registers[4] = 20000;
    while (!go.load())
      std::this_thread::yield();
    for (;;) {
      const auto result = fixture.run_on(*engine, cpu, 1u << 20);
      if (result.reason == ExitReason::system_call)
        break;
    }
  };
  const auto plain_worker = [&] {
    const auto engine = Fixture::new_engine();
    auto cpu = Fixture::task(false, 64);
    cpu.registers[0] = data_base;
    cpu.registers[1] = 7;
    cpu.registers[4] = 20000;
    while (!go.load())
      std::this_thread::yield();
    for (;;) {
      const auto result = fixture.run_on(*engine, cpu, 1u << 20);
      if (result.reason == ExitReason::system_call)
        break;
    }
  };
  BigThread first{atomic_worker};
  BigThread second{plain_worker};
  go = true;
  first.join();
  second.join();
  require(fixture.read32(0) == 20000);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  // The two-thread tests run one engine per thread.
  require(uml::cpu::arm64::set_jit_cache_budget(256U * 1024U * 1024U, 3));
  monitor_rules(0x1a32'0405);
  atomic_add_loop_fuses_and_counts(0x1a32'0401);
  conditional_store_compare_and_swap(0x1a32'0402);
  thumb_fused_add(0x1a32'0403);
  branch_out_of_the_run(0x1a32'0410);
  thumb_branch_out_of_the_run(0x1a32'0420);
  byte_and_halfword_runs(0x1a32'0404);
  faults_and_alignment(0x1a32'0406);
  two_threads_count_exactly(0x1a32'0407);
  two_threads_through_kuser_cmpxchg(0x1a32'040a);
  ordinary_store_beats_the_reservation(0x1a32'0408);
  std::puts("arm64-jit-a32-exclusive: ok");
  return 0;
}
