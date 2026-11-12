// SPDX-License-Identifier: GPL-2.0-or-later
/* The Armv8 acquire/release group on the JIT, A32 and T32: LDA and STL with
 * their byte and halfword forms (always alignment-checked), LDAEX and STLEX
 * through the fused LL/SC run and through the slow helper, the doubleword
 * pair, SEVL, and the kernel's kuser helpers that use them. The words were
 * assembled with LLVM MC at authoring time. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"

#include <cstdio>
#include <cstdlib>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

namespace aarch32 = ::uml::cpu::arm64::aarch32;

constexpr unsigned ne = 1;
constexpr std::uint32_t arm_svc = 0xef00'0000U;
constexpr std::uint16_t thumb_svc = 0xdf00U;

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

// A32.
constexpr std::uint32_t a_lda_r1 = 0xe190'1c9fU;    // lda r1, [r0]
constexpr std::uint32_t a_ldab_r2 = 0xe1d6'2c9fU;   // ldab r2, [r6]
constexpr std::uint32_t a_ldah_r3 = 0xe1f7'3c9fU;   // ldah r3, [r7]
constexpr std::uint32_t a_stl_r5 = 0xe180'fc95U;    // stl r5, [r0]
constexpr std::uint32_t a_stlb_r5 = 0xe1c6'fc95U;   // stlb r5, [r6]
constexpr std::uint32_t a_stlh_r5 = 0xe1e7'fc95U;   // stlh r5, [r7]
constexpr std::uint32_t a_ldaex_r2 = 0xe190'2e9fU;  // ldaex r2, [r0]
constexpr std::uint32_t a_add_r2_1 = 0xe282'2001U;  // add r2, r2, #1
constexpr std::uint32_t a_stlex_r3 = 0xe180'3e92U;  // stlex r3, r2, [r0]
constexpr std::uint32_t a_cmp_r3_0 = 0xe353'0000U;  // cmp r3, #0
constexpr std::uint32_t a_sevl = 0xe320'f005U;      // sevl

/* The shape of a fused run: the number of guest instructions the first block
 * holds. */
std::size_t block_instructions(Fixture &fixture, std::size_t offset,
                               std::size_t units, bool thumb) {
  std::vector<std::byte> bytes(units * (thumb ? 2 : 4));
  fixture.read_code(offset, bytes);
  const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
            : key::a32_key(code_base + static_cast<std::uint32_t>(offset)),
      bytes);
  return compiled.code.guest_instructions;
}

/* Whether the program ran without a trip to the interpreter helper: LDA and
 * STL are lowered to ldar and stlr in line. */
struct ExitCount final {
  ExitCount() {
    aarch32::reset_a32_library_exits();
    aarch32::count_a32_library_exits(true);
  }
  ~ExitCount() { aarch32::count_a32_library_exits(false); }
  [[nodiscard]] static std::size_t total() { return aarch32::a32_library_exit_total(); }
};

void round_trip(unsigned long generation, bool thumb) {
  Fixture fixture{generation};
  if (thumb)
    fixture.code16(0, {0xe8c0, 0x5faf,  // stl r5, [r0]
                       0xe8d0, 0x1faf,  // lda r1, [r0]
                       0xe8c6, 0x5f8f,  // stlb r5, [r6]
                       0xe8d6, 0x2f8f,  // ldab r2, [r6]
                       0xe8c7, 0x5f9f,  // stlh r5, [r7]
                       0xe8d7, 0x3f9f,  // ldah r3, [r7]
                       thumb_svc});
  else
    fixture.code32(0, {a_stl_r5, a_lda_r1, a_stlb_r5, a_ldab_r2, a_stlh_r5,
                       a_ldah_r3, arm_svc});
  // The neighbours of each location are not touched.
  for (std::size_t word = 0; word < 6; ++word)
    fixture.data32(word * 4, 0xa5a5'a5a5U);
  auto cpu = Fixture::task(thumb);
  cpu.registers[0] = data_base;
  cpu.registers[5] = 0x89ab'cdefU;
  cpu.registers[6] = data_base + 9;  // an odd byte
  cpu.registers[7] = data_base + 14; // a halfword
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 7);
  require(ExitCount::total() == 0);
  require(cpu.registers[1] == 0x89ab'cdefU);
  require(cpu.registers[2] == 0xefU);   // a byte, zero-extended
  require(cpu.registers[3] == 0xcdefU); // a halfword, zero-extended
  require(fixture.read32(0) == 0x89ab'cdefU);
  require(fixture.read32(8) == 0xa5a5'efa5U);  // byte 9
  require(fixture.read32(12) == 0xcdef'a5a5U); // halfword at 14
  require(fixture.read32(16) == 0xa5a5'a5a5U);
}

void alignment_faults(unsigned long generation, bool thumb) {
  struct Case final {
    std::uint32_t arm;
    std::uint16_t high, low;
    std::uint32_t address;  // r0, relative to the data base
    bool faults;
  };
  // Unlike LDR and STR, LDA and STL fault on an unaligned address whatever
  // SCTLR.A says. The byte forms cannot.
  const Case cases[] = {
      {0xe190'1c9fU, 0xe8d0, 0x1faf, 2, true}, // lda +2
      {0xe190'1c9fU, 0xe8d0, 0x1faf, 1, true}, // lda +1
      {0xe190'1c9fU, 0xe8d0, 0x1faf, 4, false}, // lda +4
      {0xe1f0'1c9fU, 0xe8d0, 0x1f9f, 1, true}, // ldah +1
      {0xe1f0'1c9fU, 0xe8d0, 0x1f9f, 2, false}, // ldah +2
      {0xe1d0'1c9fU, 0xe8d0, 0x1f8f, 1, false}, // ldab +1
      {0xe180'fc91U, 0xe8c0, 0x1faf, 2, true}, // stl +2
      {0xe180'fc91U, 0xe8c0, 0x1faf, 4, false}, // stl +4
      {0xe1e0'fc91U, 0xe8c0, 0x1f9f, 1, true}, // stlh +1
      {0xe1e0'fc91U, 0xe8c0, 0x1f9f, 2, false}, // stlh +2
      {0xe1c0'fc91U, 0xe8c0, 0x1f8f, 3, false}, // stlb +3
      {0xe1b0'4e9fU, 0xe8d0, 0x45ff, 4, true}, // ldaexd +4
      {0xe1b0'4e9fU, 0xe8d0, 0x45ff, 8, false}, // ldaexd +8
      {0xe190'1e9fU, 0xe8d0, 0x1fef, 2, true}, // ldaex +2
      {0xe1f0'1e9fU, 0xe8d0, 0x1fdf, 1, true}, // ldaexh +1
  };
  Fixture fixture{generation};
  for (const auto &c : cases) {
    if (thumb)
      fixture.code16(0, {c.high, c.low, thumb_svc});
    else
      fixture.code32(0, {c.arm, arm_svc});
    for (std::size_t word = 0; word < 6; ++word)
      fixture.data32(word * 4, 0x5a5a'5a5aU);
    auto cpu = Fixture::task(thumb);
    cpu.registers[0] = data_base + c.address;
    cpu.registers[1] = 0x1111'1111U;
    const auto result = fixture.run(cpu);
    if (c.faults) {
      require(result.reason == ExitReason::memory_fault);
      require(result.fault.alignment);
      require(result.fault.address == data_base + c.address);
      require(result.retired_instructions == 0);
      require(fixture.read32(0) == 0x5a5a'5a5aU);
      require(fixture.read32(4) == 0x5a5a'5a5aU);
    } else {
      require(result.reason == ExitReason::system_call);
      require(result.retired_instructions == 2);
    }
  }
}

void unmapped_is_not_an_alignment_fault(unsigned long generation) {
  Fixture fixture{generation};
  for (const auto word : {a_lda_r1, a_stl_r5}) {
    fixture.code32(0, {word, arm_svc});
    auto cpu = Fixture::task(false);
    cpu.registers[0] = 0x6000'0000U;
    cpu.registers[5] = 1;
    cpu.registers[1] = 0x2222'2222U;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(!result.fault.alignment);
    require(result.fault.address == 0x6000'0000U);
    require(result.retired_instructions == 0);
    require(cpu.registers[1] == 0x2222'2222U);
  }
}

/* ldaex/add/stlex/cmp/bne: the whole run is one block and one host
 * load-acquire-exclusive and store-release-exclusive pair. */
void atomic_add_loop(unsigned long generation, bool thumb) {
  Fixture fixture{generation};
  if (thumb)
    fixture.code16(0, {0xe8d0, 0x2fef,  // 1: ldaex r2, [r0]
                       0xf102, 0x0201,  // add.w r2, r2, #1
                       0xe8c0, 0x2fe3,  // stlex r3, r2, [r0]
                       0x2b00,          // cmp r3, #0
                       thumb_branch(ne, 7, 0), thumb_svc});
  else
    fixture.code32(0, {a_ldaex_r2, a_add_r2_1, a_stlex_r3, a_cmp_r3_0,
                       arm_branch(ne, 4, 0), arm_svc});
  fixture.data32(0, 5);
  auto cpu = Fixture::task(thumb);
  cpu.registers[0] = data_base;
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(ExitCount::total() == 0);
  require(fixture.read32(0) == 6);
  require(cpu.registers[2] == 6 && cpu.registers[3] == 0);
  require(block_instructions(fixture, 0, thumb ? 9 : 6, thumb) == 5);
}

void byte_and_halfword_loops(unsigned long generation) {
  Fixture fixture{generation};
  // 1: ldaexb r2,[r0]; add r2,r2,#1; stlexb r3,r2,[r0]; cmp r3,#0; bne 1b
  // 2: ldaexh r5,[r0]; add r5,r5,#1; stlexh r6,r5,[r0]; cmp r6,#0; bne 2b
  fixture.code32(0, {0xe1d0'2e9fU, a_add_r2_1, 0xe1c0'3e92U, a_cmp_r3_0,
                     arm_branch(ne, 4, 0),
                     0xe1f0'5e9fU, 0xe285'5001U, 0xe1e0'6e95U, 0xe356'0000U,
                     arm_branch(ne, 9, 5), arm_svc});
  fixture.data32(0, 0x1234'56ffU);
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base;
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(ExitCount::total() == 0);
  // The byte wraps to 0; the halfword 0x5600 + 1 follows it.
  require(cpu.registers[3] == 0 && cpu.registers[6] == 0);
  require(fixture.read32(0) == 0x1234'5601U);
  require(block_instructions(fixture, 0, 5, false) == 5);
  require(block_instructions(fixture, 20, 5, false) == 5);
}

/* The pair forms and the monitor rules go through the slow helper. */
void pair_and_monitor(unsigned long generation, bool thumb) {
  Fixture fixture{generation};
  // 1: ldaexd r4,r5,[r0]; adds r4,r4,#1; adc r5,r5,#0;
  //    stlexd r3,r4,r5,[r0]; cmp r3,#0; bne 1b
  if (thumb)
    fixture.code16(0, {0xe8d0, 0x45ff,  // ldaexd r4, r5, [r0]
                       0xf114, 0x0401,  // adds.w r4, r4, #1
                       0xf145, 0x0500,  // adc r5, r5, #0
                       0xe8c0, 0x45f3,  // stlexd r3, r4, r5, [r0]
                       0x2b00, thumb_branch(ne, 9, 0), thumb_svc});
  else
    fixture.code32(0, {0xe1b0'4e9fU, 0xe294'4001U, 0xe2a5'5000U, 0xe1a0'3e94U,
                       a_cmp_r3_0, arm_branch(ne, 5, 0), arm_svc});
  fixture.data32(0, 0xffff'ffffU);
  fixture.data32(4, 0x0000'0001U);
  auto cpu = Fixture::task(thumb);
  cpu.registers[0] = data_base;
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[3] == 0);
  require(fixture.read32(0) == 0 && fixture.read32(4) == 2);
  require(cpu.registers[4] == 0 && cpu.registers[5] == 2);

  // A store-release-exclusive with no reservation fails; one after a plain
  // store in between fails; one straight after the load succeeds.
  const auto run = [&](std::initializer_list<std::uint32_t> program) {
    fixture.code32(0, program);
    auto task = Fixture::task(false);
    task.registers[0] = data_base;
    task.registers[2] = 99;
    task.registers[5] = 99;
    const auto stopped = fixture.run(task);
    require(stopped.reason == ExitReason::system_call);
    return task;
  };
  // A fused pair is the host's own load and store exclusive, and the host
  // store fails when anything clears the host monitor in between, such as an
  // interrupt or a context switch. The architecture allows that failure and
  // a guest retries, so a pair that must succeed gets a few attempts.
  const auto run_until_stored = [&](std::initializer_list<std::uint32_t> program) {
    for (int attempt = 0; attempt < 16; ++attempt) {
      fixture.data32(0, 10);
      auto task = run(program);
      if (task.registers[3] == 0)
        return task;
    }
    require(false);
    return Fixture::task(false);
  };
  fixture.data32(0, 10);
  cpu = run({a_stlex_r3, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 10);
  fixture.data32(0, 10);
  cpu = run({a_ldaex_r2, 0xe580'5000U /* str r5,[r0] */, a_stlex_r3, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 99);
  cpu = run_until_stored({a_ldaex_r2, a_stlex_r3, arm_svc});
  require(fixture.read32(0) == 10);
  // CLREX takes the reservation away.
  fixture.data32(0, 10);
  cpu = run({a_ldaex_r2, 0xf57f'f01fU, a_stlex_r3, arm_svc});
  require(cpu.registers[3] == 1 && fixture.read32(0) == 10);
  // An LDREX pairs with an STLEX and an LDAEX with an STREX.
  cpu = run_until_stored({0xe190'2f9fU, a_stlex_r3, arm_svc});
  require(fixture.read32(0) == 10);
  cpu = run_until_stored(
      {a_ldaex_r2, 0xe180'3f92U /* strex r3,r2,[r0] */, arm_svc});
  require(fixture.read32(0) == 10);
}

/* SEVL is a no-op for the flags and the registers and retires like any
 * instruction. */
void sevl_is_a_no_op(unsigned long generation) {
  Fixture fixture{generation};
  fixture.code32(0, {0xe350'0001U /* cmp r0, #1 */, a_sevl,
                     0xe3a0'1007U /* mov r1, #7 */, arm_svc});
  auto cpu = Fixture::task(false);
  cpu.registers[0] = 1;
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require((cpu.nzcv & flag_z) != 0 && (cpu.nzcv & flag_c) != 0);
  require(cpu.registers[1] == 7);
  fixture.code16(0, {0x2107 /* movs r1, #7 */, 0x2801 /* cmp r0, #1 */,
                     0xf3af, 0x8005 /* sevl.w */, thumb_svc});
  cpu = Fixture::task(true);
  cpu.registers[0] = 1;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require((cpu.nzcv & flag_z) != 0);
  require(cpu.registers[1] == 7);
  // Under a condition that fails it does nothing either.
  fixture.code32(0, {0xe350'0001U, 0x1320'f005U /* sevlne */, arm_svc});
  cpu = Fixture::task(false);
  cpu.registers[0] = 1;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 3);
}

/* The kernel's kuser helper page, word for word (arm64 Linux's kuser32.S), at
 * the offsets the kernel gives them in a read-only page at 0xffff0000. */
constexpr std::uint32_t kuser_page = 0xffff'0000U;
constexpr std::size_t kuser_cmpxchg64 = 0xf60;
constexpr std::size_t kuser_memory_barrier = 0xfa0;
constexpr std::size_t kuser_cmpxchg = 0xfc0;
constexpr std::size_t kuser_get_tls = 0xfe0;

void map_kuser_page(Fixture &fixture) {
  fixture.map_code_alias(kuser_page);
  fixture.code32(kuser_cmpxchg64, {
      0xe92d'00f0U, // push {r4, r5, r6, r7}
      0xe1c0'40d0U, // ldrd r4, r5, [r0]
      0xe1c1'60d0U, // ldrd r6, r7, [r1]
      0xe1b2'0f9fU, // 1: ldrexd r0, r1, [r2]
      0xe030'3004U, // eors r3, r0, r4
      0x0031'3005U, // eoreqs r3, r1, r5
      0x01a2'3e96U, // stlexdeq r3, r6, [r2]
      0x0333'0001U, // teqeq r3, #1
      0x0aff'fff9U, // beq 1b
      0xf57f'f05bU, // dmb ish
      0xe273'0000U, // rsbs r0, r3, #0
      0xe8bd'00f0U, // pop {r4, r5, r6, r7}
      0xe12f'ff1eU, // bx lr
  });
  fixture.code32(kuser_memory_barrier, {0xf57f'f05bU, 0xe12f'ff1eU});
  fixture.code32(kuser_cmpxchg, {
      0xe192'3f9fU, // 1: ldrex r3, [r2]
      0xe053'3000U, // subs r3, r3, r0
      0x0182'3e91U, // stlexeq r3, r1, [r2]
      0x0333'0001U, // teqeq r3, #1
      0x0aff'fffaU, // beq 1b
      0xf57f'f05bU, // dmb ish
      0xe273'0000U, // rsbs r0, r3, #0
      0xe12f'ff1eU, // bx lr
  });
  fixture.code32(kuser_get_tls, {
      0xee1d'0f70U, // mrc p15, 0, r0, c13, c0, 3
      0xe12f'ff1eU, // bx lr
  });
  fixture.code32(64, {arm_svc});
}

[[nodiscard]] arm64::CpuState kuser_call(std::size_t helper) {
  auto cpu = Fixture::task(false);
  cpu.pc = key::a32_key(kuser_page + static_cast<std::uint32_t>(helper));
  cpu.registers[14] = code_base + 64;
  return cpu;
}

void kuser_cmpxchg_through_stlex(unsigned long generation) {
  Fixture fixture{generation};
  map_kuser_page(fixture);
  for (const bool match : {true, false}) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      fixture.data32(0, 0x1234);
      auto cpu = kuser_call(kuser_cmpxchg);
      cpu.registers[0] = match ? 0x1234 : 0x4321; // the old value
      cpu.registers[1] = 0x5678;                  // the new one
      cpu.registers[2] = data_base;
      const auto result = fixture.run(cpu);
      require(result.reason == ExitReason::system_call);
      // r0 is 0 when the swap happened, nonzero otherwise.
      require((cpu.registers[0] == 0) == match);
      require(fixture.read32(0) == (match ? 0x5678U : 0x1234U));
    }
  }
}

void kuser_cmpxchg64_through_stlexd(unsigned long generation) {
  Fixture fixture{generation};
  map_kuser_page(fixture);
  for (const bool match : {true, false}) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      fixture.data32(0, 0x0102'0304U);  // the 64-bit word at the data base
      fixture.data32(4, 0x0506'0708U);
      fixture.data32(16, match ? 0x0102'0304U : 0x0102'0305U); // the old value
      fixture.data32(20, 0x0506'0708U);
      fixture.data32(24, 0xa1a2'a3a4U); // the new one
      fixture.data32(28, 0xb1b2'b3b4U);
      auto cpu = kuser_call(kuser_cmpxchg64);
      cpu.registers[0] = data_base + 16;
      cpu.registers[1] = data_base + 24;
      cpu.registers[2] = data_base;
      const auto result = fixture.run(cpu);
      require(result.reason == ExitReason::system_call);
      require((cpu.registers[0] == 0) == match);
      require(fixture.read32(0) == (match ? 0xa1a2'a3a4U : 0x0102'0304U));
      require(fixture.read32(4) == (match ? 0xb1b2'b3b4U : 0x0506'0708U));
      // The helper restores the registers it saved.
      require(cpu.registers[13] == data_base + guest_page);
    }
  }
}

void kuser_barrier_and_tls(unsigned long generation) {
  Fixture fixture{generation};
  map_kuser_page(fixture);
  auto cpu = kuser_call(kuser_memory_barrier);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 3);
  cpu = kuser_call(kuser_get_tls);
  cpu.a32_tpidruro = 0x4000'1230U;
  cpu.registers[0] = 0;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == 0x4000'1230U);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  require(uml::cpu::arm64::set_jit_cache_budget(256U * 1024U * 1024U, 2));
  unsigned long generation = 0x1a32'0501;
  round_trip(generation++, false);
  round_trip(generation++, true);
  alignment_faults(generation++, false);
  alignment_faults(generation++, true);
  unmapped_is_not_an_alignment_fault(generation++);
  atomic_add_loop(generation++, false);
  atomic_add_loop(generation++, true);
  byte_and_halfword_loops(generation++);
  pair_and_monitor(generation++, false);
  pair_and_monitor(generation++, true);
  sevl_is_a_no_op(generation++);
  kuser_cmpxchg_through_stlex(generation++);
  kuser_cmpxchg64_through_stlexd(generation++);
  kuser_barrier_and_tls(generation++);
  std::puts("arm64-jit-a32-acquire-release: ok");
  return 0;
}
