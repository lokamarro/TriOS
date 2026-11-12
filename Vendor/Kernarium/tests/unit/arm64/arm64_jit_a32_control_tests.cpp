// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's AArch32 control behaviour: IT blocks entered from the start, one
 * instruction at a time and from the middle; blocks longer than the compiler
 * keeps in one translation and the instruction budget across them; rewritten
 * code (A32 words, T32 halfwords of a 32-bit instruction, one byte) seen by
 * the translations that held the old code; and the complete level's optional
 * extensions, which decode only where the host has them.
 *
 * Random A32 and T32 programs run too: conditional branches, forward and back
 * (B, CBZ, CBNZ), among data processing, whole and one instruction at a time;
 * loops with memory, Advanced SIMD and side exits in their bodies that end by
 * a count, a branch out or a fault; and the counted loop's translation as a
 * host loop through the chain budget. The shapes of blocks are pinned: one
 * that goes on past a branch back, one that ends at a branch to its own first
 * instruction, and a linked block (the fifteen core registers loaded once
 * before its internal entry, none written back before a link), with the
 * registers across the budget returns of a chain with a call and a return.
 * For one form of each optional extension, hosts with every extension and
 * hosts lacking one are compared: the form is UNDEFINED, A32 and T32, and
 * nothing else changes. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;
using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;

namespace {

/* cmp r4, #0; ittte eq; moveq r0,#1; moveq r1,#2; moveq r2,#3; movne r3,#4;
 * ite ne; addne r5,#1; addeq r5,#2; svc. */
constexpr std::array<std::uint16_t, 10> it_program{
    0x2c00, 0xbf03, 0x2001, 0x2102, 0x2203, 0x2304, 0xbf14, 0x1c6d, 0x1cad, 0xdf00};
/* The second conditional instruction's offset, in bytes. */
constexpr unsigned second_move = 6;

arm64::CpuState it_task(std::uint32_t r4) {
  auto cpu = Fixture::task(true);
  cpu.registers[4] = r4;
  cpu.registers[5] = 100;
  cpu.registers[3] = 0x33;
  return cpu;
}

void check_final(const arm64::CpuState &cpu, bool equal) {
  require(cpu.registers[0] == (equal ? 1U : 0U));
  require(cpu.registers[1] == (equal ? 2U : 0U));
  require(cpu.registers[2] == (equal ? 3U : 0U));
  require(cpu.registers[3] == (equal ? 0x33U : 4U));
  require(cpu.registers[5] == (equal ? 102U : 101U));
  require(cpu.a32_itstate == 0);
  require(cpu.pc == key::t32_key(code_base + 2 * it_program.size()));
}

void it_blocks_whole_stepped_and_entered_mid_block() {
  Fixture fixture{0x1a32'0c01};
  for (std::size_t index = 0; index < it_program.size(); ++index)
    fixture.code16(2 * index, {it_program[index]});

  for (const bool equal : {true, false}) {
    // The whole block.
    auto cpu = it_task(equal ? 0 : 1);
    auto result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    require(result.retired_instructions == it_program.size());
    check_final(cpu, equal);

    // One instruction at a time: ITSTATE is live exactly inside the blocks.
    cpu = it_task(equal ? 0 : 1);
    std::vector<std::uint8_t> itstates;
    std::size_t retired = 0;
    for (;;) {
      result = fixture.run(cpu, 1, true);
      retired += result.retired_instructions;
      if (result.reason == ExitReason::system_call)
        break;
      require(result.reason == ExitReason::single_step);
      require(result.retired_instructions == 1);
      itstates.push_back(cpu.a32_itstate);
      require(itstates.size() < 20);
    }
    require(retired == it_program.size());
    check_final(cpu, equal);
    require(itstates.size() == it_program.size() - 1);
    require(itstates[0] == 0);   // after the cmp
    require(itstates[1] != 0);   // after ittte
    require(itstates[2] != 0);   // after the first move
    require(itstates[3] != 0);   // after the second
    require(itstates[4] != 0);   // after the third
    require(itstates[5] == 0);   // after the last of the block
    require(itstates[6] != 0);   // after ite
    require(itstates[7] != 0);
    require(itstates[8] == 0);

    // Entered in the middle of the first block: the state a step observed.
    cpu = it_task(equal ? 0 : 1);
    cpu.nzcv = equal ? flag_z : 0;
    cpu.pc = key::t32_key(code_base + second_move);
    cpu.a32_itstate = itstates[2];
    result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    require(result.retired_instructions == it_program.size() - 3);
    require(cpu.registers[0] == 0); // the first move was not executed
    require(cpu.registers[1] == (equal ? 2U : 0U));
    require(cpu.registers[2] == (equal ? 3U : 0U));
    require(cpu.registers[3] == (equal ? 0x33U : 4U));
    require(cpu.registers[5] == (equal ? 102U : 101U));
    require(cpu.a32_itstate == 0);
  }
}

void long_blocks_and_the_budget() {
  // Long enough that a chain (which carries on across the blocks of a straight
  // run, their exits being linked, until its allowance of 4096 instructions is
  // spent) ends before the program does. The code spans 12 pages in either
  // profile.
  Fixture fixture{0x1a32'0c02, 12};
  constexpr std::size_t adds = 10000;
  {
    std::vector<std::uint32_t> words(adds, 0xe280'0001U); // add r0, r0, #1
    words.push_back(0xef00'0000U);
    for (std::size_t index = 0; index < words.size(); ++index)
      fixture.code32(4 * index, {words[index]});
  }
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu, 1u << 20);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == adds + 1);
  require(cpu.registers[0] == adds);

  // A small budget stops at a block boundary, past the budget, with exactly
  // the retired instructions' effects, and the next call resumes there.
  cpu = Fixture::task(false);
  std::size_t retired = 0;
  unsigned exits = 0;
  for (;;) {
    result = fixture.run(cpu, 50);
    retired += result.retired_instructions;
    if (result.reason == ExitReason::system_call)
      break;
    require(result.reason == ExitReason::instruction_budget);
    require(result.retired_instructions >= 50);
    require(cpu.registers[0] == retired);
    require(cpu.pc == key::a32_key(code_base + 4 * static_cast<std::uint32_t>(retired)));
    require(++exits < 1000);
  }
  require(exits >= 2);
  require(retired == adds + 1);
  require(cpu.registers[0] == adds);
}

/* The core registers stay in the host from one block to the next of a chain,
 * through the direct links of a call and the indirect link of its return, and
 * are in the CpuState whenever the chain ends. A chain ends when its allowance
 * of instructions is spent, at whichever link it has reached; the registers
 * are then what the retired instructions made them, and at the end what the
 * whole program did. */
void registers_survive_budget_returns_in_a_chain() {
  Fixture fixture{0x1a32'0c15, 2, 2};
  constexpr std::uint32_t inner = 200, outer = 100;
  const std::vector<std::uint32_t> program{
      0xe3a0'7000U, //  0: mov r7, #0
      0xe3a0'5000U, //  4: mov r5, #0              (outer)
      0xe280'0001U, //  8: add r0, r0, #1          (inner)
      0xe281'1002U, // 12: add r1, r1, #2
      0xe282'2003U, // 16: add r2, r2, #3
      0xeb00'0008U, // 20: bl +32 -> 60
      0xe285'5001U, // 24: add r5, r5, #1
      0xe355'0000U | inner, // 28: cmp r5, #200
      0x1aff'fff8U, // 32: bne -32 -> 8
      0xe287'7001U, // 36: add r7, r7, #1
      0xe357'0000U | outer, // 40: cmp r7, #100
      0x1aff'fff4U, // 44: bne -48 -> 4
      0xe3a0'6009U, // 48: mov r6, #9
      0xef00'0000U, // 52: svc
      0xe320'f000U, // 56: nop
      0xe283'3001U, // 60: add r3, r3, #1        (the callee)
      0xe284'4005U, // 64: add r4, r4, #5
      0xe12f'ff1eU, // 68: bx lr
  };
  for (std::size_t i = 0; i < program.size(); ++i)
    fixture.code32(i * 4, {program[i]});
  auto cpu = Fixture::task(false);
  unsigned exits = 0;
  for (;;) {
    const auto result = fixture.run(cpu, 4096);
    if (result.reason == ::uml::cpu::ExitReason::system_call)
      break;
    require(result.reason == ::uml::cpu::ExitReason::instruction_budget);
    // A chain ends at a block boundary: the callee's counts follow the
    // caller's.
    require(cpu.registers[3] <= cpu.registers[0]);
    require(cpu.registers[0] <= cpu.registers[3] + 1U);
    require(cpu.registers[4] == 5U * cpu.registers[3]);
    require(++exits < 5000);
  }
  require(exits >= 20);
  require(cpu.registers[0] == inner * outer);
  require(cpu.registers[1] == 2U * inner * outer);
  require(cpu.registers[2] == 3U * inner * outer);
  require(cpu.registers[3] == inner * outer);
  require(cpu.registers[4] == 5U * inner * outer);
  require(cpu.registers[5] == inner);
  require(cpu.registers[6] == 9U);
  require(cpu.registers[7] == outer);
}

/* A linked block loads the fifteen core registers once, before its internal
 * entry (which a linked predecessor skips), and a linked exit writes none of
 * them back: the registers are written back only where the block returns to
 * the executor, in its cold paths, and then as pairs. */
void linked_blocks_keep_the_registers_in_the_host() {
  Fixture fixture{0x1a32'0c16, 2, 2};
  fixture.code32(0, {0xe280'0001U,  // add r0, r0, #1
                     0xe081'1000U,  // add r1, r1, r0
                     0xe7e0'0050U,  // ubfx r0, r0, #0, #1 (a bit of data work)
                     0xea00'0010U}); // b +72
  std::vector<std::byte> bytes(4 * 4);
  fixture.read_code(0, bytes);
  const auto linked = ::uml::cpu::arm64::jit::testing::compile_a32_linked_block(
      key::a32_key(code_base), bytes);
  const auto unlinked = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      key::a32_key(code_base), bytes);
  require(!linked.code.words.empty() && !unlinked.code.words.empty());
  // ldr/str xN, [x15, #imm] and ldp/stp xN, xM, [x15, #imm] of the register
  // file, in the words from `first` up to `last`.
  const auto count = [](const auto &code, std::size_t first, std::size_t last,
                        bool load) {
    unsigned transfers = 0;
    for (std::size_t i = first; i < last && i < code.words.size(); ++i) {
      const auto w = code.words[i];
      const bool single = (w & 0xffc0'03e0U) ==
                          ((load ? 0xf940'0000U : 0xf900'0000U) | 15U << 5);
      const bool pair = (w & 0xffc0'03e0U) ==
                        ((load ? 0xa940'0000U : 0xa900'0000U) | 15U << 5);
      if ((single && ((w >> 10) & 0xfffU) < 15U) ||
          (pair && ((w >> 15) & 0x7fU) < 15U))
        ++transfers;
    }
    return transfers;
  };
  const auto &code = linked.code;
  // The branch into the successor.
  std::size_t jump = 0;
  while (jump < code.words.size() &&
         (code.words[jump] & 0xffff'fc1fU) != 0xd61f'0000U)
    ++jump;
  require(jump < code.hot_words);
  // Eight loads (seven pairs and a single) before the internal entry...
  require(count(code, 0, code.internal_entry_word, true) == 8U);
  // ...none after it, and no write-back before the branch into the successor.
  require(count(code, code.internal_entry_word, code.hot_words, true) == 0U);
  require(count(code, code.internal_entry_word, jump, false) == 0U);
  // The return that follows a refused link writes all fifteen back: seven
  // pairs and a single.
  require(count(code, jump, code.hot_words, false) == 8U);
  // The unlinked block loads what it uses and writes back what it changed
  // before the exit.
  require(count(unlinked.code, unlinked.code.internal_entry_word,
                unlinked.code.hot_words, false) != 0U);
}

/* Whether a program's blocks are one chain once they are all translated: the
 * blocks are run last first, so each one's exit finds a translated successor,
 * and the whole program then runs from its start as a single block entry. The
 * exits that join them are static: the fetch window's cut, the arm of a
 * conditional branch or CBZ that is not taken. */
struct ChainProgram final {
  std::vector<std::uint8_t> bytes;
  std::vector<std::size_t> starts; // offsets of the blocks after the first
  bool thumb;
  std::size_t retired;
  std::uint32_t r0;
};

void run_as_one_chain(unsigned long generation, const ChainProgram &program) {
  constexpr std::uint32_t base = 0x00f0'0000U;
  JitBacking backing{1};
  backing.put(0, std::as_bytes(std::span{program.bytes}));
  JitMapping mapping{generation, base, backing, 1};
  ::uml::cpu::arm64::JitExecutor executor;
  ::uml::memory::StaticGuestMemory memory;
  const auto entry = [&](std::size_t offset) {
    arm64::CpuState cpu{};
    cpu.a32 = 1;
    cpu.pc = program.thumb ? key::t32_key(base + static_cast<std::uint32_t>(offset))
                           : key::a32_key(base + static_cast<std::uint32_t>(offset));
    return cpu;
  };
  require(executor.begin_execution(generation, base));
  std::size_t executed = 0;
  for (auto start = program.starts.rbegin(); start != program.starts.rend();
       ++start) {
    auto cpu = entry(*start);
    (void)executor.run_a32_block(cpu, memory, generation, executed);
  }
  auto cpu = entry(0);
  const auto event = executor.run_a32_block(cpu, memory, generation, executed);
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(executed == program.retired);
  require(cpu.registers[0] == program.r0);
  executor.end_execution();
}

/* A chain is entered with what has been switched on, not only what its first
 * block needs. Once a block of floating-point code has switched the native
 * environment on, a chain that begins at a block without any links into one
 * with it, and the whole program is one entry: before, the selector of the
 * first block (no floating point) refused the link, and the loop of an
 * integer block and a floating-point block returned to the executor at every
 * edge. */
void native_environment_is_carried_into_a_chain() {
  constexpr std::uint32_t base = 0x00f0'0000U;
  constexpr std::size_t window = 64; // the compiler's A32 block length
  constexpr std::uint32_t add_r0 = 0xe280'0001U; // add r0, r0, #1
  constexpr std::uint32_t vadd = 0xee30'0a20U;   // vadd.f32 s0, s0, s1
  std::vector<std::uint32_t> words(window, add_r0);
  for (unsigned i = 0; i < 5; ++i)
    words.push_back(vadd);
  words.push_back(0xef00'0000U); // svc
  std::vector<std::uint8_t> bytes;
  for (const auto word : words)
    for (unsigned shift = 0; shift < 32; shift += 8)
      bytes.push_back(static_cast<std::uint8_t>(word >> shift));
  constexpr unsigned long generation = 0x1a32'0c20;
  JitBacking backing{1};
  backing.put(0, std::as_bytes(std::span{bytes}));
  JitMapping mapping{generation, base, backing, 1};
  ::uml::cpu::arm64::JitExecutor executor;
  ::uml::memory::StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.a32 = 1;
  require(executor.begin_execution(generation, base));
  std::size_t executed = 0;
  // The floating-point block alone: it switches the environment on.
  cpu.pc = key::a32_key(base + static_cast<std::uint32_t>(window * 4));
  require(executor.run_a32_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == 6);
  require(cpu.gadget_fp_active);
  // The integer block first, then the translated floating-point one.
  cpu.pc = key::a32_key(base);
  const auto event = executor.run_a32_block(cpu, memory, generation, executed);
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(executed == words.size());
  require(cpu.registers[0] == window);
  executor.end_execution();
}

/* A function called from five places returns to five different targets: the
 * two cells of its return's site cannot hold them, and each return missed
 * and went back to the executor. The executor's table of trained targets
 * holds all five, so once they have been seen a run of the program is one
 * entry. */
void polymorphic_returns_stay_in_the_chain() {
  constexpr std::uint32_t base = 0x00f0'0000U;
  constexpr unsigned long generation = 0x1a32'0c21;
  constexpr unsigned sites = 5;
  for (const bool thumb : {false, true}) {
    std::vector<std::uint8_t> bytes;
    const auto put16 = [&](std::uint32_t half) {
      bytes.push_back(static_cast<std::uint8_t>(half));
      bytes.push_back(static_cast<std::uint8_t>(half >> 8));
    };
    const auto put32 = [&](std::uint32_t word) {
      put16(word & 0xffffU);
      put16(word >> 16);
    };
    for (unsigned i = 0; i < sites; ++i) {
      if (thumb) {
        // bl f, with f at byte 24: the offset from the PC four bytes on.
        put16(0xf000U);
        put16(0xf800U | ((24U - 4U * i - 4U) >> 1));
      } else {
        put32(0xeb00'0000U | (4U - i)); // bl f
      }
    }
    if (thumb) {
      put16(0xdf00U); // svc 0
      put16(0xbf00U); // nop
      put16(0x3001U); // adds r0, r0, #1
      put16(0x4770U); // bx lr
    } else {
      put32(0xef00'0000U); // svc
      put32(0xe280'0001U); // f: add r0, r0, #1
      put32(0xe12f'ff1eU); // bx lr
    }
    JitBacking backing{1};
    backing.put(0, std::as_bytes(std::span{bytes}));
    JitMapping mapping{generation + (thumb ? 1U : 0U), base, backing, 1};
    ::uml::cpu::arm64::JitExecutor executor;
    ::uml::memory::StaticGuestMemory memory;
    arm64::CpuState cpu{};
    cpu.a32 = 1;
    const auto g = generation + (thumb ? 1U : 0U);
    require(executor.begin_execution(g, base));
    constexpr unsigned rounds = 8;
    unsigned calls = 0;
    for (unsigned round = 0; round < rounds; ++round) {
      cpu.pc = thumb ? key::t32_key(base) : key::a32_key(base);
      calls = 0;
      std::size_t executed = 0;
      ::uml::cpu::EngineEvent event;
      do {
        event = executor.run_a32_block(cpu, memory, g, executed);
        require(++calls < 64);
      } while (event == ::uml::cpu::EngineEvent::continued);
      require(event == ::uml::cpu::EngineEvent::system_call);
      require(cpu.registers[0] == sites * (round + 1U));
    }
    require(calls == 1U);
    executor.end_execution();
  }
}

/* The table of indirect targets belongs to the executor, which serves many
 * address spaces in turn: the same guest key in two of them is two different
 * translations. Two address spaces with the same function at the same address
 * that add different amounts run alternately, each call site returning to the
 * same targets in both; every run must see its own function. */
void indirect_targets_are_per_address_space() {
  constexpr std::uint32_t base = 0x00f0'0000U;
  constexpr unsigned long first = 0x1a32'0c30;
  constexpr unsigned long second = 0x1a32'0c31;
  constexpr unsigned sites = 5;
  const auto program = [&](std::uint32_t amount) {
    std::vector<std::uint8_t> bytes;
    const auto put32 = [&](std::uint32_t word) {
      for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    };
    for (unsigned i = 0; i < sites; ++i)
      put32(0xeb00'0000U | (4U - i));      // bl f
    put32(0xef00'0000U);                   // svc
    put32(0xe280'0000U | amount);          // f: add r0, r0, #amount
    put32(0xe12f'ff1eU);                   // bx lr
    return bytes;
  };
  const auto one = program(1), hundred = program(100);
  JitBacking backing_one{1}, backing_hundred{1};
  backing_one.put(0, std::as_bytes(std::span{one}));
  backing_hundred.put(0, std::as_bytes(std::span{hundred}));
  JitMapping mapping_one{first, base, backing_one, 1};
  JitMapping mapping_hundred{second, base, backing_hundred, 1};
  ::uml::cpu::arm64::JitExecutor executor;
  ::uml::memory::StaticGuestMemory memory;
  arm64::CpuState cpu_one{}, cpu_hundred{};
  cpu_one.a32 = 1;
  cpu_hundred.a32 = 1;
  for (unsigned round = 0; round < 12; ++round) {
    for (const bool use_first : {true, false}) {
      auto &cpu = use_first ? cpu_one : cpu_hundred;
      const auto generation = use_first ? first : second;
      require(executor.begin_execution(generation, base));
      cpu.pc = key::a32_key(base);
      const auto before = cpu.registers[0];
      std::size_t executed = 0;
      unsigned calls = 0;
      ::uml::cpu::EngineEvent event;
      do {
        event = executor.run_a32_block(cpu, memory, generation, executed);
        require(++calls < 64);
      } while (event == ::uml::cpu::EngineEvent::continued);
      require(event == ::uml::cpu::EngineEvent::system_call);
      require(cpu.registers[0] - before == sites * (use_first ? 1U : 100U));
      executor.end_execution();
    }
  }
}

/* The host has a register for each of the guest's fifteen core registers, so
 * a block that names thirteen of them reads each from the register file once
 * and writes each back once, not at every use of those past the eighth. */
void core_registers_stay_in_the_host() {
  Fixture fixture{0x1a32'0c20, 2, 2};
  std::vector<std::uint32_t> words;
  for (unsigned i = 0; i < 39; ++i)
    words.push_back(0xe080'0000U | ((i + 1U) % 13U) << 16 | (i % 13U) << 12 |
                    ((i + 5U) % 13U)); // add rd, rn, rm
  words.push_back(0xef00'0000U);       // svc
  for (std::size_t i = 0; i < words.size(); ++i)
    fixture.code32(i * 4, {words[i]});
  std::vector<std::byte> bytes(words.size() * 4);
  fixture.read_code(0, bytes);
  const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      key::a32_key(code_base), bytes);
  require(!compiled.code.words.empty());
  unsigned loads = 0, stores = 0;
  for (const auto w : compiled.code.words) {
    // ldr/str xN, [x15, #imm] of one of the first fifteen doublewords
    if ((w & 0xffc0'03e0U) == (0xf940'0000U | 15U << 5) &&
        ((w >> 10) & 0xfffU) < 15U)
      ++loads;
    if ((w & 0xffc0'03e0U) == (0xf900'0000U | 15U << 5) &&
        ((w >> 10) & 0xfffU) < 15U)
      ++stores;
  }
  require(loads <= 13U && loads >= 1U);
  require(stores <= 13U && stores >= 1U);
}

void static_exits_chain() {
  constexpr std::uint32_t add_r0 = 0xe280'0001U; // add r0, r0, #1
  const auto a32_bytes = [](const std::vector<std::uint32_t> &words) {
    std::vector<std::uint8_t> bytes;
    for (const auto word : words)
      for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    return bytes;
  };

  // A straight run of more words than a block holds: the fetch window cuts it.
  // An A32 block holds maximum_block_instructions; a T32 block four fewer,
  // the room an IT block opened by its last instruction may need
  // (a32_compiler.cpp).
  {
    constexpr std::size_t window = 64;
    constexpr std::size_t words = 2 * window + 22;
    std::vector<std::uint32_t> program(words, add_r0);
    program.push_back(0xef00'0000U); // svc
    run_as_one_chain(0x1a32'0c10, {a32_bytes(program), {window * 4, 2 * window * 4},
                                   false, words + 1, static_cast<std::uint32_t>(words)});
  }
  {
    constexpr std::size_t window = 60;
    constexpr std::size_t halves = 2 * window + 22;
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i <= halves; ++i) {
      const std::uint16_t half = i < halves ? 0x3001 : 0xdf00; // adds r0, #1; svc
      bytes.push_back(static_cast<std::uint8_t>(half));
      bytes.push_back(static_cast<std::uint8_t>(half >> 8));
    }
    run_as_one_chain(0x1a32'0c19, {bytes, {window * 2, 2 * window * 2}, true,
                                   halves + 1, static_cast<std::uint32_t>(halves)});
  }
  // A conditional branch that is not taken: cmp r1, r1 sets Z, so bne falls
  // through to the next block; the skipped arm is a static successor.
  {
    const std::uint32_t cmp = 0xe151'0001U;  // cmp r1, r1
    const std::uint32_t bne = 0x1a00'0000U;  // bne .+8
    run_as_one_chain(
        0x1a32'0c11,
        {a32_bytes({cmp, bne, add_r0, cmp, bne, add_r0, cmp, bne, add_r0, 0xef00'0000U}),
         {8, 20, 32},
         false, 10, 3});
  }
  // CBZ that is not taken (T32): the fall-through is a static successor.
  {
    const std::vector<std::uint16_t> halves{
        0x2101,  // movs r1, #1
        0xb101,  // cbz r1, .+4 (not taken)
        0x3001,  // adds r0, #1
        0xb101,  // cbz r1, .+4
        0x3001,  // adds r0, #1
        0xdf00,  // svc
    };
    std::vector<std::uint8_t> bytes;
    for (const auto half : halves) {
      bytes.push_back(static_cast<std::uint8_t>(half));
      bytes.push_back(static_cast<std::uint8_t>(half >> 8));
    }
    run_as_one_chain(0x1a32'0c12, {bytes, {4, 8}, true, 6, 2});
  }
}

/* mov r0, #1 (T32 mov.w), nop, movw r8, #0x1234, svc. */
void rewritten_t32_halfwords() {
  Fixture fixture{0x1a32'0c03};
  fixture.code16(0, {0x2001, 0xbf00, 0xf241, 0x2834, 0xdf00});
  auto cpu = Fixture::task(true);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1234);

  // The second halfword of a 32-bit instruction alone.
  fixture.code16(6, {0x2835});
  cpu = Fixture::task(true);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1235);

  // The first halfword alone: movw r8 becomes movt r8 (0xf2c1).
  fixture.code16(4, {0xf2c1});
  cpu = Fixture::task(true);
  cpu.registers[8] = 0x0000'00aaU;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1235'00aaU);

  // A nop rewritten into the first half of a 32-bit instruction changes the
  // length of what follows it: movw r8, #0x1236, then cmp r0, #0x35.
  fixture.code16(2, {0xf241, 0x2836});
  cpu = Fixture::task(true);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require(cpu.registers[8] == 0x1236);
  require((cpu.nzcv & flag_n) != 0); // the old second half is now cmp r0, #0x35
}

void rewritten_a32_words() {
  Fixture fixture{0x1a32'0c04};
  fixture.code32(0, {0xe3a0'0001U, 0xe280'1002U, 0xe281'2003U, 0xef00'0000U});
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[2] == 6);

  fixture.code32(4, {0xe280'1010U}); // add r1, r0, #16
  cpu = Fixture::task(false);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[1] == 17 && cpu.registers[2] == 20);

  // Only the last byte of the word changes (the condition field): the
  // instruction becomes a conditional one that does not execute.
  fixture.code_byte(11, std::byte{0x02}); // addeq r2, r1, #3
  cpu = Fixture::task(false);
  cpu.registers[2] = 0x77;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[2] == 0x77);
}

} // namespace

/* An entry whose first fetch is unmapped faults at the PC with an execute
 * access and retires nothing, in either instruction set. */
void unmapped_entry() {
  Fixture fixture{0x1a32'0c05};
  for (const bool thumb : {false, true}) {
    auto cpu = Fixture::task(thumb);
    cpu.pc = thumb ? key::t32_key(0x0070'0000U) : key::a32_key(0x0070'0000U);
    const auto before = cpu.pc;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.fault.address == 0x0070'0000U);
    require(result.fault.access == ::uml::memory::MemoryAccess::execute);
    require(result.retired_instructions == 0);
    require(cpu.pc == before);
  }
}

/* A step over BX into Thumb state and one over BX back leave the PC key of
 * the target, tag included, and the next step runs there. */
void single_step_over_interworking_branches() {
  Fixture fixture{0x1a32'0c06};
  fixture.code32(0, {0xe28f'2004U,  // add r2, pc, #4
                     0xe382'2001U,  // orr r2, r2, #1
                     0xe12f'ff12U}); // bx r2
  fixture.code16(12, {0x2007,       // movs r0, #7
                      0xdf00});     // svc #0
  auto cpu = Fixture::task(false);
  for (unsigned step = 0; step < 3; ++step) {
    const auto result = fixture.run(cpu, 1, true);
    require(result.reason == ExitReason::single_step);
    require(result.retired_instructions == 1);
  }
  require(cpu.registers[2] == code_base + 12 + 1);
  require(cpu.pc == key::t32_key(code_base + 12));
  auto result = fixture.run(cpu, 1, true);
  require(result.reason == ExitReason::single_step);
  require(cpu.registers[0] == 7);
  require(cpu.pc == key::t32_key(code_base + 14));
  result = fixture.run(cpu, 1, true);
  require(result.reason == ExitReason::system_call);
  require(cpu.pc == key::t32_key(code_base + 16));

  // BX the other way: Thumb to A32, one step.
  fixture.code16(32, {0x4778,   // bx pc (to the A32 word after the nop)
                      0xbf00});
  fixture.code32(36, {0xe3a0'1009U}); // mov r1, #9
  cpu = Fixture::task(true, 32);
  auto step = fixture.run(cpu, 1, true);
  require(step.reason == ExitReason::single_step);
  require(cpu.pc == key::a32_key(code_base + 36));
  step = fixture.run(cpu, 1, true);
  require(step.reason == ExitReason::single_step);
  require(cpu.registers[1] == 9);
}

/* The optional extensions of the complete level, by the A64 host feature
 * each is shown for. */
enum class Extension {
  crc32, aes, sha1, sha2, fp16, dotprod, fhm, bf16, i8mm, sb, rdm, fcma, jscvt,
};

/* One instruction of an extension, as an A32 word and as the T32 word
 * (halfword one in the upper half). */
struct ExtensionForm {
  const char *name;
  Extension extension;
  std::uint32_t a32;
  std::uint32_t t32;
};

constexpr std::array extension_forms{
    ExtensionForm{"CRC32W", Extension::crc32, 0xe1410042U, 0xfac1f0a2U},
    ExtensionForm{"AESE", Extension::aes, 0xf3b00302U, 0xffb00302U},
    ExtensionForm{"SHA1C", Extension::sha1, 0xf2020c44U, 0xef020c44U},
    ExtensionForm{"SHA256H", Extension::sha2, 0xf3020c44U, 0xff020c44U},
    ExtensionForm{"VADD.F16", Extension::fp16, 0xf2110d02U, 0xef110d02U},
    ExtensionForm{"VSDOT", Extension::dotprod, 0xfc210d02U, 0xfc210d02U},
    ExtensionForm{"VFMAL", Extension::fhm, 0xfc200891U, 0xfc200891U},
    ExtensionForm{"VDOT.BF16", Extension::bf16, 0xfc010d02U, 0xfc010d02U},
    ExtensionForm{"VUSDOT", Extension::i8mm, 0xfca10d02U, 0xfca10d02U},
    ExtensionForm{"SB", Extension::sb, 0xf57ff070U, 0xf3bf8f70U},
    ExtensionForm{"VQRDMLAH", Extension::rdm, 0xf3110b12U, 0xff110b12U},
    ExtensionForm{"VCADD", Extension::fcma, 0xfc910802U, 0xfc910802U},
    ExtensionForm{"VJCVT", Extension::jscvt, 0xeeb90bc1U, 0xeeb90bc1U},
};
/* VMAXNM Dd, Dn, Dm: the Armv8 floating-point additions, which every host
 * has and the host features do not gate. */
constexpr ExtensionForm always_present{"VMAXNM", Extension::crc32, 0xf3010f12U,
                                       0xff010f12U}; // its extension unused

/* The host as probed with every extension on, less `lacking` when it is
 * given: the A64 features a32_features reads. */
HostCapabilities extension_host(std::optional<Extension> lacking) {
  HostCapabilities host = ::uml::cpu::host_capabilities();
  host.advanced_simd = true;
  host.floating_point = true;
  const auto feature = [&host, lacking](Extension extension, ArmFeature which,
                                        bool *flag = nullptr) {
    const bool on = lacking != extension;
    if (flag != nullptr)
      *flag = on;
    if (on)
      host.supported.set(static_cast<std::size_t>(which));
    else
      host.supported.reset(static_cast<std::size_t>(which));
  };
  feature(Extension::crc32, ArmFeature::crc32, &host.crc32);
  feature(Extension::aes, ArmFeature::aes, &host.aes);
  feature(Extension::fp16, ArmFeature::fp16, &host.fp16);
  host.pmull = true;
  host.sha1 = lacking != Extension::sha1;
  host.sha2 = lacking != Extension::sha2;
  feature(Extension::dotprod, ArmFeature::dotprod);
  feature(Extension::fhm, ArmFeature::fhm);
  feature(Extension::bf16, ArmFeature::bf16);
  feature(Extension::i8mm, ArmFeature::i8mm);
  feature(Extension::sb, ArmFeature::sb);
  feature(Extension::rdm, ArmFeature::rdm);
  feature(Extension::fcma, ArmFeature::fcma);
  feature(Extension::jscvt, ArmFeature::jscvt);
  return host;
}

/* Runs each form, A32 and T32, at its own offset followed by an SVC, and
 * says whether it reached the SVC (true) or the illegal-instruction exit
 * (false). */
bool extension_form_runs(Fixture &fixture, std::size_t slot,
                         const ExtensionForm &form, bool thumb) {
  const std::size_t offset = slot * 16;
  if (thumb)
    fixture.code16(offset, {static_cast<std::uint16_t>(form.t32 >> 16),
                            static_cast<std::uint16_t>(form.t32), 0xdf00});
  else
    fixture.code32(offset, {form.a32, 0xef000000U});
  auto cpu = Fixture::task(thumb, offset);
  const auto result = fixture.run(cpu);
  if (result.reason == ExitReason::system_call)
    return true;
  if (result.reason != ExitReason::illegal_instruction) {
    std::fprintf(stderr, "%s %s: unexpected exit %d\n", form.name,
                 thumb ? "T32" : "A32", static_cast<int>(result.reason));
    require(false);
  }
  return false;
}

/* An extension decodes only where the host has its A64 feature, which is
 * what the guest is shown: on a host without it the form is UNDEFINED, not a
 * form the semantic library runs, and nothing else changes. */
void extensions_decode_only_where_the_host_has_them() {
  unsigned long generation = 0x1a32'0d00;
  {
    const auto host = extension_host(std::nullopt);
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    Fixture fixture{generation++};
    std::size_t slot = 0;
    for (const auto &form : extension_forms)
      for (const bool thumb : {false, true}) {
        if (!extension_form_runs(fixture, slot++, form, thumb)) {
          std::fprintf(stderr, "%s %s is UNDEFINED on a host with it\n",
                       form.name, thumb ? "T32" : "A32");
          require(false);
        }
      }
  }
  for (const auto &lacking : extension_forms) {
    const auto host = extension_host(lacking.extension);
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    Fixture fixture{generation++};
    std::size_t slot = 0;
    for (const auto &form : extension_forms)
      for (const bool thumb : {false, true}) {
        const bool runs = extension_form_runs(fixture, slot++, form, thumb);
        if (runs != (form.extension != lacking.extension)) {
          std::fprintf(stderr, "%s %s %s on a host without %s\n", form.name,
                       thumb ? "T32" : "A32", runs ? "runs" : "is UNDEFINED",
                       lacking.name);
          require(false);
        }
      }
    for (const bool thumb : {false, true})
      require(extension_form_runs(fixture, slot++, always_present, thumb));
  }
}

// ---------------------------------------------------------------------------
// A block goes on past a conditional forward branch (B with a condition, CBZ,
// CBNZ) and leaves by a side exit when it is taken. Random programs of those
// branches among data processing, run as the translator forms them and one
// instruction at a time (which never goes on past a branch), must end in the
// same registers, flags and count.

class Lcg final {
public:
  explicit Lcg(std::uint64_t seed) : state_{seed | 1U} {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  unsigned below(unsigned bound) { return static_cast<unsigned>(next() % bound); }

private:
  std::uint64_t state_;
};

struct Outcome final {
  arm64::CpuState cpu;
  std::size_t retired;
  ::uml::cpu::ExitReason reason;
};

Outcome run_whole(Fixture &fixture, arm64::CpuState cpu) {
  const auto result = fixture.run(cpu, 4096);
  return {cpu, result.retired_instructions, result.reason};
}

Outcome run_stepped(Fixture &fixture, arm64::CpuState cpu) {
  std::size_t retired = 0;
  for (unsigned step = 0; step < 4096; ++step) {
    const auto result = fixture.run(cpu, 1, true);
    retired += result.retired_instructions;
    if (result.reason != ExitReason::single_step)
      return {cpu, retired, result.reason};
  }
  return {cpu, retired, ExitReason::single_step};
}

Outcome run_whole_long(Fixture &fixture, arm64::CpuState cpu) {
  const auto result = fixture.run(cpu, 1u << 22);
  return {cpu, result.retired_instructions, result.reason};
}

Outcome run_stepped_long(Fixture &fixture, arm64::CpuState cpu) {
  std::size_t retired = 0;
  for (unsigned step = 0; step < 400000; ++step) {
    const auto result = fixture.run(cpu, 1, true);
    retired += result.retired_instructions;
    if (result.reason != ExitReason::single_step)
      return {cpu, retired, result.reason};
  }
  return {cpu, retired, ExitReason::single_step};
}

void require_same(const Outcome &whole, const Outcome &stepped,
                  const char *what, unsigned program) {
  bool same = whole.reason == stepped.reason &&
              whole.retired == stepped.retired &&
              whole.cpu.pc == stepped.cpu.pc &&
              (whole.cpu.nzcv & 0xf000'0000U) == (stepped.cpu.nzcv & 0xf000'0000U);
  for (unsigned i = 0; i < 15; ++i)
    same = same && whole.cpu.registers[i] == stepped.cpu.registers[i];
  if (!same) {
    std::fprintf(stderr, "%s program %u: whole retired=%zu pc=%llx nzcv=%08x "
                         "stepped retired=%zu pc=%llx nzcv=%08x\n",
                 what, program, whole.retired,
                 static_cast<unsigned long long>(whole.cpu.pc),
                 static_cast<unsigned>(whole.cpu.nzcv), stepped.retired,
                 static_cast<unsigned long long>(stepped.cpu.pc),
                 static_cast<unsigned>(stepped.cpu.nzcv));
    for (unsigned i = 0; i < 15; ++i)
      std::fprintf(stderr, "  r%u whole=%08x stepped=%08x\n", i,
                   static_cast<unsigned>(whole.cpu.registers[i]),
                   static_cast<unsigned>(stepped.cpu.registers[i]));
  }
  require(same);
}

arm64::CpuState random_registers(Lcg &random, bool thumb) {
  auto cpu = Fixture::task(thumb);
  for (unsigned i = 0; i < 13; ++i) {
    static constexpr std::uint32_t special[] = {0, 1, 2, 0x7fff'ffffU,
                                                0x8000'0000U, 0xffff'ffffU};
    cpu.registers[i] = random.below(3) == 0
                           ? special[random.below(std::size(special))]
                           : static_cast<std::uint32_t>(random.next());
  }
  cpu.nzcv = static_cast<std::uint32_t>(random.next()) & 0xf000'0000U;
  return cpu;
}

void side_exit_branches() {
  constexpr unsigned programs = 120;
  Fixture fixture{0x1a32'0c0a, 24};
  Lcg random{0x51de'e817};
  for (const bool thumb : {false, true}) {
    for (unsigned p = 0; p < programs; ++p) {
      // A third of the programs are longer than a block holds.
      const unsigned length = p % 3 == 0 ? 90 + random.below(30) : 12 + random.below(40);
      const std::size_t unit = thumb ? 2 : 4;
      const std::size_t offset = static_cast<std::size_t>(p % 40) * 512;
      std::vector<std::uint32_t> words(length);
      std::vector<std::uint16_t> halves(length);
      for (unsigned i = 0; i < length; ++i) {
        const unsigned rd = random.below(12), rn = random.below(12);
        const bool branch = random.below(4) == 0 && i + 2 < length;
        // A forward branch skips 1 to 4 instructions, or all that remain.
        const unsigned skip = std::min<unsigned>(1 + random.below(4), length - i - 1);
        if (!thumb) {
          static constexpr unsigned ops[] = {0, 1, 2, 3, 4, 12, 13, 14};
          const unsigned op = ops[random.below(std::size(ops))];
          const unsigned cond = random.below(10) == 0 ? random.below(15)
                                                      : 14;
          if (branch) {
            // The target is skip + 1 instructions on: PC is two ahead.
            const unsigned bcond = random.below(8) == 0 ? 14 : random.below(14);
            words[i] = bcond << 28 | 0x0a00'0000U | (skip - 1U);
          } else {
            const bool s = random.below(2) != 0;
            // MOV's Rn field is zero.
            words[i] = cond << 28 | 0x0200'0000U | op << 21 |
                       (s ? 1U << 20 : 0U) | (op == 13U ? 0U : rn) << 16 |
                       rd << 12 | random.below(256);
          }
        } else {
          const unsigned lo = random.below(8), lo2 = random.below(8),
                         lo3 = random.below(8);
          if (branch && random.below(3) == 0) {
            // CBZ/CBNZ Rn, target: PC is two halfwords ahead, imm6 = skip - 1.
            const unsigned imm6 = skip - 1U;
            halves[i] = static_cast<std::uint16_t>(
                0xb100U | (random.below(2) != 0 ? 0x0800U : 0U) |
                (imm6 >> 5 & 1U) << 9 | (imm6 & 31U) << 3 | lo);
          } else if (branch) {
            halves[i] = static_cast<std::uint16_t>(
                0xd000U | random.below(14) << 8 | (skip - 1U));
          } else {
            switch (random.below(8)) {
            case 0: halves[i] = static_cast<std::uint16_t>(0x2000U | lo << 8 | random.below(256)); break;
            case 1: halves[i] = static_cast<std::uint16_t>(0x1c00U | random.below(8) << 6 | lo2 << 3 | lo); break;
            case 2: halves[i] = static_cast<std::uint16_t>(0x1a00U | lo3 << 6 | lo2 << 3 | lo); break;
            case 3: halves[i] = static_cast<std::uint16_t>(0x1800U | lo3 << 6 | lo2 << 3 | lo); break;
            case 4: halves[i] = static_cast<std::uint16_t>(0x2800U | lo << 8 | random.below(256)); break;
            case 5: halves[i] = static_cast<std::uint16_t>(0x4000U | lo2 << 3 | lo); break;
            case 6: halves[i] = static_cast<std::uint16_t>(0x4040U | lo2 << 3 | lo); break;
            default: halves[i] = static_cast<std::uint16_t>(random.below(32) << 6 | lo2 << 3 | lo); break;
            }
          }
        }
      }
      for (unsigned i = 0; i < length; ++i) {
        if (thumb)
          fixture.code16(offset + i * unit, {halves[i]});
        else
          fixture.code32(offset + i * unit, {words[i]});
      }
      if (thumb)
        fixture.code16(offset + length * unit, {0xdf00U});
      else
        fixture.code32(offset + length * unit, {0xef00'0000U});
      for (unsigned round = 0; round < 6; ++round) {
        auto cpu = random_registers(random, thumb);
        cpu.pc = thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
                       : key::a32_key(code_base + static_cast<std::uint32_t>(offset));
        const auto whole = run_whole(fixture, cpu);
        require(whole.reason == ExitReason::system_call);
        const auto stepped = run_stepped(fixture, cpu);
        require_same(whole, stepped, thumb ? "t32" : "a32", p);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// A block that ends in a backward branch to its own start loops in the host.
// Random loops (data processing under conditions, loads and stores through a
// fixed and a walking base, Advanced SIMD, forward branches out of the body,
// a counted back edge that is conditional or an unconditional one left by a
// branch), run whole and one instruction at a time, must end in the same
// registers, vectors, flags, memory and count, whether they end at the
// instruction that follows the loop or in a fault in an iteration the host
// loop ran.

struct Item final {
  std::vector<std::uint16_t> halves;
  int target = -1; // a branch: the item index it goes to
  enum class Kind : std::uint8_t { plain, cond_branch, back_branch } kind{};
  unsigned cond = 14;
};

std::uint16_t thumb_first(std::uint32_t word) {
  return static_cast<std::uint16_t>(word >> 16);
}
std::uint16_t thumb_second(std::uint32_t word) {
  return static_cast<std::uint16_t>(word);
}
/* An A32 Advanced SIMD word as a T32 word. */
std::uint32_t to_thumb_neon(std::uint32_t word) {
  if ((word & 0xff00'0000U) == 0xf400'0000U)
    return 0xf900'0000U | (word & 0x00ff'ffffU);
  return 0xef00'0000U | (word & 0x00ff'ffffU);
}

std::uint32_t neon_vadd_i32(unsigned qd, unsigned qn, unsigned qm) {
  const unsigned d = 2 * qd, n = 2 * qn, m = 2 * qm;
  return 0xf220'0840U | (d >> 4 & 1U) << 22 | (n & 15U) << 16 | (d & 15U) << 12 |
         (n >> 4 & 1U) << 7 | (m >> 4 & 1U) << 5 | (m & 15U);
}
std::uint32_t neon_vld1_q(unsigned qd, unsigned rn) {
  const unsigned d = 2 * qd;
  return 0xf420'0a8fU | (d >> 4 & 1U) << 22 | rn << 16 | (d & 15U) << 12;
}
std::uint32_t neon_vst1_q(unsigned qd, unsigned rn) {
  const unsigned d = 2 * qd;
  return 0xf400'0a8fU | (d >> 4 & 1U) << 22 | rn << 16 | (d & 15U) << 12;
}

/* The translation of a counted loop is a loop in the host: a budget
 * decrement of the block's length followed by a branch backwards, and
 * nothing of the descriptor-linked exit the loop would otherwise take. */
bool has_host_loop(const std::vector<std::uint32_t> &words, std::size_t length) {
  const std::uint32_t decrement = 0xd100'0000U | static_cast<std::uint32_t>(length) << 10 |
                                  20U << 5 | 20U;
  for (std::size_t i = 0; i + 1 < words.size(); ++i)
    if (words[i] == decrement && (words[i + 1] & 0xfc00'0000U) == 0x1400'0000U &&
        (words[i + 1] & 0x0200'0000U) != 0U)
      return true;
  return false;
}

void counted_loop_is_a_host_loop() {
  Fixture fixture{0x1a32'0c0c, 4};
  // 1: add r0,r0,r1; ldr r2,[r10]; subs r12,r12,#1; bne 1b; svc
  fixture.code32(0, {0xe080'0001U, 0xe59a'2000U, 0xe25c'c001U,
                     0x1aff'fffbU, 0xef00'0000U});
  std::vector<std::byte> bytes(5 * 4);
  fixture.read_code(0, bytes);
  const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      key::a32_key(code_base), bytes);
  require(compiled.code.guest_instructions == 4);
  require(has_host_loop(compiled.code.words, 4));
  // The same loop in T32.
  fixture.code16(64, {0x4408, 0xf8da, 0x2000, 0xf1bc, 0x0c01, 0xd1f9, 0xdf00});
  std::vector<std::byte> thumb(7 * 2);
  fixture.read_code(64, thumb);
  const auto compiled_thumb = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      key::t32_key(code_base + 64), thumb);
  require(compiled_thumb.code.guest_instructions == 4);
  require(has_host_loop(compiled_thumb.code.words, 4));
  // It runs through budget windows: 3000 iterations of four instructions.
  auto cpu = Fixture::task(false);
  cpu.registers[0] = 5;
  cpu.registers[1] = 3;
  cpu.registers[10] = data_base;
  cpu.registers[12] = 3000;
  fixture.data32(0, 0x1234);
  const auto result = fixture.run(cpu, 1u << 22);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == 5U + 3U * 3000U);
  require(cpu.registers[2] == 0x1234U);
  require(cpu.registers[12] == 0);
  require(result.retired_instructions == 4U * 3000U + 1U);
  // A limit that falls inside the loop stops it at the end of the chain
  // window past it, in the state of that many single steps.
  for (const std::size_t limit : {1U, 3U, 4U, 5U, 7U, 399U, 1001U, 4095U, 4096U,
                                  4097U, 4099U, 4100U, 8000U, 8190U}) {
    auto start = Fixture::task(false);
    start.registers[0] = 5;
    start.registers[1] = 3;
    start.registers[10] = data_base;
    start.registers[12] = 3000;
    auto whole = start;
    const auto limited = fixture.run(whole, limit);
    require(limited.reason == ExitReason::instruction_budget);
    // A chain runs its whole allowance of 4096 instructions and stops at an
    // iteration boundary.
    require(limited.retired_instructions >= limit &&
            limited.retired_instructions < limit + 4096 &&
            limited.retired_instructions % 4096 < 4);
    auto stepped = start;
    for (std::size_t step = 0; step < limited.retired_instructions; ++step)
      (void)fixture.run(stepped, 1, true);
    require(whole.pc == stepped.pc);
    for (unsigned i = 0; i < 15; ++i)
      require(whole.registers[i] == stepped.registers[i]);
    require((whole.nzcv & 0xf000'0000U) == (stepped.nzcv & 0xf000'0000U));
  }
}

/* A block goes on past a conditional branch back as it does past one forward:
 * a `continue` is rarely taken and leaves by a side exit. A branch back to the
 * block's own first instruction still ends the block, which the host loop
 * covers. */
void backward_branches_are_side_exits() {
  Fixture fixture{0x1a32'0c17, 2, 2};
  const auto units = [&](const std::vector<std::uint32_t> &program) {
    for (std::size_t i = 0; i < program.size(); ++i)
      fixture.code32(i * 4, {program[i]});
    std::vector<std::byte> bytes(program.size() * 4);
    fixture.read_code(0, bytes);
    return ::uml::cpu::arm64::jit::testing::compile_a32_block(
               key::a32_key(code_base), bytes)
        .guest_units;
  };
  // add; add; cmp; bne back to the second add; add; svc: the block goes on.
  require(units({0xe280'0001U, 0xe281'1001U, 0xe350'0005U, 0x1aff'fffcU,
                 0xe282'2001U, 0xef00'0000U}) == 6U);
  // The same branch to the first instruction ends the block.
  require(units({0xe280'0001U, 0xe281'1001U, 0xe350'0005U, 0x1aff'fffbU,
                 0xe282'2001U, 0xef00'0000U}) == 4U);
}

void backward_side_exit_programs() {
  constexpr unsigned programs = 160;
  Fixture fixture{0x1a32'0c18, 24};
  Lcg random{0xbac4'7a2d};
  unsigned completed = 0;
  for (const bool thumb : {false, true}) {
    for (unsigned p = 0; p < programs; ++p) {
      const unsigned length = 10 + random.below(40);
      const std::size_t unit = thumb ? 2 : 4;
      const std::size_t offset = static_cast<std::size_t>(p % 40) * 512;
      for (unsigned i = 0; i < length; ++i) {
        const unsigned rd = random.below(12), rn = random.below(12);
        // A branch back lands 1 to 6 instructions earlier (not before the
        // first); the others are data processing.
        const bool branch = random.below(5) == 0 && i >= 2;
        const unsigned back = std::min<unsigned>(1 + random.below(6), i);
        if (!thumb) {
          static constexpr unsigned ops[] = {0, 1, 2, 3, 4, 12, 13, 14};
          const unsigned op = ops[random.below(std::size(ops))];
          if (branch) {
            // PC is two ahead: the target is `back` instructions earlier.
            const std::uint32_t imm24 = (0U - (back + 2U)) & 0x00ff'ffffU;
            const unsigned bcond = random.below(14);
            fixture.code32(offset + i * unit,
                           {bcond << 28 | 0x0a00'0000U | imm24});
          } else {
            const bool s = random.below(2) != 0;
            const unsigned cond = random.below(10) == 0 ? random.below(15) : 14;
            fixture.code32(offset + i * unit,
                           {cond << 28 | 0x0200'0000U | op << 21 |
                            (s ? 1U << 20 : 0U) | (op == 13U ? 0U : rn) << 16 |
                            rd << 12 | random.below(256)});
          }
        } else {
          const unsigned lo = random.below(8), lo2 = random.below(8),
                         lo3 = random.below(8);
          std::uint16_t half;
          if (branch) {
            // PC is two halfwords ahead.
            const std::uint32_t imm8 = (0U - (back + 2U)) & 0xffU;
            half = static_cast<std::uint16_t>(0xd000U | random.below(14) << 8 | imm8);
          } else {
            switch (random.below(8)) {
            case 0: half = static_cast<std::uint16_t>(0x2000U | lo << 8 | random.below(256)); break;
            case 1: half = static_cast<std::uint16_t>(0x1c00U | random.below(8) << 6 | lo2 << 3 | lo); break;
            case 2: half = static_cast<std::uint16_t>(0x1a00U | lo3 << 6 | lo2 << 3 | lo); break;
            case 3: half = static_cast<std::uint16_t>(0x1800U | lo3 << 6 | lo2 << 3 | lo); break;
            case 4: half = static_cast<std::uint16_t>(0x2800U | lo << 8 | random.below(256)); break;
            case 5: half = static_cast<std::uint16_t>(0x4000U | lo2 << 3 | lo); break;
            case 6: half = static_cast<std::uint16_t>(0x4040U | lo2 << 3 | lo); break;
            default: half = static_cast<std::uint16_t>(random.below(32) << 6 | lo2 << 3 | lo); break;
            }
          }
          fixture.code16(offset + i * unit, {half});
        }
      }
      if (thumb)
        fixture.code16(offset + length * unit, {0xdf00U});
      else
        fixture.code32(offset + length * unit, {0xef00'0000U});
      for (unsigned round = 0; round < 6; ++round) {
        auto cpu = random_registers(random, thumb);
        cpu.pc = thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
                       : key::a32_key(code_base + static_cast<std::uint32_t>(offset));
        // A loop that does not end within the budget is not compared: its
        // stop is at a block boundary, which differs from a single step's.
        const auto whole = run_whole(fixture, cpu);
        if (whole.reason != ExitReason::system_call)
          continue;
        const auto stepped = run_stepped(fixture, cpu);
        require_same(whole, stepped, thumb ? "t32" : "a32", p);
        ++completed;
      }
    }
  }
  require(completed >= programs);
}

/* The inline TLB probe is the short form: the entry index extracted from the
 * address (one instruction) and a tag compare that shifts the address itself,
 * not the A64 compiler's longer sequence. A probe with the wrong index would
 * only miss, so the words are what is checked. An access of more than one byte that may be unaligned
 * compares the tag with its last byte's page, which tells a page crossing
 * from a hit without a test of its own. */
void tlb_probe_is_the_short_form() {
  Fixture fixture{0x1a32'0c11, 2, 2};
  const auto compile = [&](std::uint32_t word) {
    fixture.code32(0, {word, 0xef00'0000U}); // the access; svc
    std::vector<std::byte> bytes(2 * 4);
    fixture.read_code(0, bytes);
    return ::uml::cpu::arm64::jit::testing::compile_a32_block(
        key::a32_key(code_base), bytes);
  };
  const auto has = [](const auto &compiled, std::uint32_t word) {
    return std::find(compiled.code.words.begin(), compiled.code.words.end(),
                     word) != compiled.code.words.end();
  };
  const auto byte_load = compile(0xe5d1'0004U); // ldrb r0,[r1,#4]
  require(has(byte_load, 0xd34c'6500U));  // ubfx x0, x8, #12, #14
  require(has(byte_load, 0xca48'3042U));  // eor x2, x2, x8, lsr #12
  require(!has(byte_load, 0xd34c'fd01U)); // lsr x1, x8, #12 (the long form)
  const auto word_load = compile(0xe591'0004U); // ldr r0,[r1,#4]
  require(has(word_load, 0xd34c'6500U));
  require(has(word_load, 0x9100'0d01U));  // add x1, x8, #3
  require(has(word_load, 0xca41'3042U));  // eor x2, x2, x1, lsr #12
  require(!has(word_load, 0xca48'3042U));
}

/* An access the hot path has checked is aligned to its size cannot span two
 * pages, so it is not tested for it: a single-precision VLDR, and a VLD1 whose
 * alignment hint covers its bytes. A word load, which may be unaligned, and a
 * VLD1 without a hint are (by the tag compare with the last byte's page). */
void aligned_accesses_skip_the_crossing_test() {
  Fixture fixture{0x1a32'0c12, 2, 2};
  const auto checks_crossing = [&](std::uint32_t word, unsigned bytes) {
    fixture.code32(0, {word, 0xef00'0000U});
    std::vector<std::byte> block(2 * 4);
    fixture.read_code(0, block);
    const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
        key::a32_key(code_base), block);
    require(!compiled.code.words.empty());
    return std::any_of(
        compiled.code.words.begin(), compiled.code.words.end(),
        [&](std::uint32_t w) {
          // add x1, x8, #(bytes - 1)
          return w == (0x9100'0101U | (bytes - 1U) << 10);
        });
  };
  require(checks_crossing(0xe591'0000U, 4));  // ldr r0,[r1]
  require(!checks_crossing(0xed91'0a00U, 4)); // vldr s0,[r1]
  require(checks_crossing(0xed91'0b00U, 8));  // vldr d0,[r1]
  require(checks_crossing(0xf421'0a8fU, 16)); // vld1.32 {d0,d1},[r1]
  require(!checks_crossing(0xf421'0aafU, 16)); // vld1.32 {d0,d1},[r1:128]
}

/* A single load or store adds the translation's delta in its own addressing
 * mode, [x9, w8, uxtw], and leaves the add of the host pointer out of the hot
 * path; a form with no such mode (a pair, a multiple, an acquire or release,
 * a store of the PC, which is read into x9) keeps the add. The accesses run
 * the same on the cold path that fills the TLB and on the hit path after it. */
void single_transfers_index_by_the_delta() {
  Fixture fixture{0x1a32'0c13, 2, 2};
  constexpr std::uint32_t add_x8_x9 = 0x8b09'0108U; // add x8, x8, x9
  const auto compile = [&](std::uint32_t word) {
    fixture.code32(0, {word, 0xef00'0000U});
    std::vector<std::byte> bytes(2 * 4);
    fixture.read_code(0, bytes);
    return ::uml::cpu::arm64::jit::testing::compile_a32_block(
        key::a32_key(code_base), bytes);
  };
  const auto folded = [&](std::uint32_t word, std::uint32_t indexed) {
    const auto compiled = compile(word);
    require(!compiled.code.words.empty());
    bool has_indexed = false, has_add = false;
    for (const auto w : compiled.code.words) {
      has_indexed |= (w & ~0x1fU) == (indexed | 8U << 16 | 9U << 5);
      has_add |= w == add_x8_x9;
    }
    return has_indexed && !has_add;
  };
  const auto unfolded = [&](std::uint32_t word) {
    const auto compiled = compile(word);
    require(!compiled.code.words.empty());
    return std::find(compiled.code.words.begin(), compiled.code.words.end(),
                     add_x8_x9) != compiled.code.words.end();
  };
  require(folded(0xe591'0004U, 0xb860'4800U)); // ldr r0,[r1,#4]
  require(folded(0xe5d1'0004U, 0x3860'4800U)); // ldrb r0,[r1,#4]
  require(folded(0xe1d1'00b4U, 0x7860'4800U)); // ldrh r0,[r1,#4]
  require(folded(0xe1d1'00d4U, 0x38e0'4800U)); // ldrsb r0,[r1,#4]
  require(folded(0xe1d1'00f4U, 0x78e0'4800U)); // ldrsh r0,[r1,#4]
  require(folded(0xe581'0004U, 0xb820'4800U)); // str r0,[r1,#4]
  require(folded(0xe5c1'0004U, 0x3820'4800U)); // strb r0,[r1,#4]
  require(folded(0xe1c1'00b4U, 0x7820'4800U)); // strh r0,[r1,#4]
  const auto q_word = [](std::uint32_t indexed) {
    return indexed | 8U << 16 | 9U << 5;
  };
  const auto folded_q = [&](std::uint32_t word, std::uint32_t indexed) {
    const auto compiled = compile(word);
    require(!compiled.code.words.empty());
    bool has_indexed = false, has_add = false;
    for (const auto w : compiled.code.words) {
      has_indexed |= (w & ~0x1fU) == q_word(indexed);
      has_add |= w == add_x8_x9;
    }
    return has_indexed && !has_add;
  };
  require(folded_q(0xf421'0a8fU, 0x3ce0'4800U)); // vld1.32 {d0,d1},[r1]
  require(folded_q(0xf401'0a8fU, 0x3ca0'4800U)); // vst1.32 {d0,d1},[r1]
  require(folded_q(0xec91'0b04U, 0x3ce0'4800U)); // vldmia r1,{d0,d1}
  require(unfolded(0xf421'078fU));             // vld1.32 {d0},[r1]
  require(unfolded(0xec91'0b08U));             // vldmia r1,{d0-d3}
  require(unfolded(0xe1c1'00d0U));             // ldrd r0,r1,[r1]
  require(unfolded(0xe891'0003U));             // ldm r1,{r0,r1}
  require(unfolded(0xe191'0c9fU));             // lda r0,[r1]
  require(unfolded(0xe581'f008U));             // str pc,[r1,#8]

  const std::vector<std::uint32_t> program{
      0xe581'2000U, // str r2, [r1]
      0xe591'0000U, // ldr r0, [r1]
      0xe5c1'2005U, // strb r2, [r1, #5]
      0xe5d1'3005U, // ldrb r3, [r1, #5]
      0xe1c1'20b6U, // strh r2, [r1, #6]
      0xe1d1'40f6U, // ldrsh r4, [r1, #6]
      0xe1d1'50d5U, // ldrsb r5, [r1, #5]
      0xe581'f008U, // str pc, [r1, #8]
      0xe591'6008U, // ldr r6, [r1, #8]
      0xe588'2000U, // str r2, [r8]
      0xe598'7000U, // ldr r7, [r8]
      0xf429'2a8fU, // vld1.32 {d2, d3}, [r9]
      0xf40a'2a8fU, // vst1.32 {d2, d3}, [r10]
      0xef00'0000U, // svc
  };
  std::array<std::byte, 16> pattern{};
  for (unsigned i = 0; i < pattern.size(); ++i)
    pattern[i] = static_cast<std::byte>(0x30U + 7U * i);
  fixture.data_bytes(0x40, pattern);
  for (std::size_t i = 0; i < program.size(); ++i)
    fixture.code32(i * 4, {program[i]});
  for (unsigned round = 0; round < 3; ++round) {
    auto cpu = Fixture::task(false);
    cpu.registers[1] = data_base;
    cpu.registers[2] = 0x8123'f0f5U;
    cpu.registers[8] = data_base + guest_page;
    cpu.registers[9] = data_base + 0x40U;
    cpu.registers[10] = data_base + guest_page + 0x20U;
    const auto result = fixture.run(cpu);
    require(result.reason == ::uml::cpu::ExitReason::system_call);
    require(cpu.registers[0] == 0x8123'f0f5U);
    require(cpu.registers[3] == 0xf5U);
    require(cpu.registers[4] == 0xffff'f0f5U);
    require(cpu.registers[5] == 0xffff'fff5U);
    require(cpu.registers[6] == code_base + 28U + 8U);
    require(cpu.registers[7] == 0x8123'f0f5U);
    require(fixture.read32(8) == code_base + 28U + 8U);
    require(fixture.read32(guest_page) == 0x8123'f0f5U);
    require(fixture.read8(5) == 0xf5U);
    const auto stored = fixture.read_data(guest_page + 0x20U, 16);
    require(std::memcmp(stored.data(), pattern.data(), 16) == 0);
    require(std::memcmp(cpu.vectors[1].bytes.data(), pattern.data(), 16) == 0);
  }
}

/* A conversion to a 16-bit fixed-point value and VJCVT keep their temporaries
 * out of the host registers that hold the guest's core registers: r0, r1 and
 * r2 are the first bound, in x3, x4 and x5, and are written before the
 * conversion and read after it. */
void conversions_keep_the_guest_registers() {
  Fixture fixture{0x1a32'0c14, 2, 2};
  const std::vector<std::uint32_t> program{
      0xe3a0'0005U, // mov r0, #5
      0xe3a0'1006U, // mov r1, #6
      0xe3a0'2007U, // mov r2, #7
      0xeebe'0a40U, // vcvt.s16.f32 s0, s0, #16
      0xeebf'0a40U, // vcvt.u16.f32 s0, s0, #16
      0xeebd'1bc0U, // vcvt.s32.f64 s2, d0
      0xeef9'0bc0U, // vjcvt.s32.f64 s1, d0
      0xe080'3001U, // add r3, r0, r1
      0xe083'3002U, // add r3, r3, r2
      0xef00'0000U, // svc
  };
  for (std::size_t i = 0; i < program.size(); ++i)
    fixture.code32(i * 4, {program[i]});
  for (unsigned round = 0; round < 2; ++round) {
    auto cpu = Fixture::task(false);
    cpu.vectors[0].bytes.fill(std::byte{0x41});
    const auto result = fixture.run(cpu);
    require(result.reason == ::uml::cpu::ExitReason::system_call);
    require(cpu.registers[0] == 5U);
    require(cpu.registers[1] == 6U);
    require(cpu.registers[2] == 7U);
    require(cpu.registers[3] == 18U);
  }
}

void native_loops() {
  constexpr unsigned programs = 90;
  constexpr std::size_t window = 256; // bytes of data the loops touch
  Fixture fixture{0x1a32'0c0b, 24};
  Lcg random{0x100b'a5e5};
  std::vector<std::byte> original(window);
  for (const bool thumb : {false, true}) {
    for (unsigned p = 0; p < programs; ++p) {
      const std::size_t offset = static_cast<std::size_t>(p % 20) * 1024;
      std::vector<Item> items;
      const unsigned body = 3 + random.below(26);
      const bool counted = random.below(2) == 0;
      for (unsigned i = 0; i < body; ++i) {
        Item item;
        const unsigned rd = random.below(10), rn = random.below(10);
        const unsigned pick = random.below(16);
        if (pick < 2 && i + 2 < body) {
          item.kind = Item::Kind::cond_branch;
          item.cond = random.below(14);
          item.target = static_cast<int>(i + 1 + random.below(3));
          item.halves.assign(thumb ? 1 : 2, 0); // laid out below
        } else if (pick < 5) {
          // A load or store through r10 at a word offset below the window.
          const unsigned off = random.below(16) * 4;
          const bool store = random.below(2) != 0;
          if (!thumb) {
            const std::uint32_t word = (store ? 0xe58a'0000U : 0xe59a'0000U) |
                                       rd << 12 | off;
            item.halves = {thumb_first(word), thumb_second(word)};
          } else {
            const std::uint32_t word = (store ? 0xf8ca'0000U : 0xf8da'0000U) |
                                       rd << 12 | off;
            item.halves = {thumb_first(word), thumb_second(word)};
          }
        } else if (pick < 6) {
          // A post-indexed access through the walking base r11.
          const bool store = random.below(2) != 0;
          if (!thumb) {
            const std::uint32_t word = (store ? 0xe48b'0004U : 0xe49b'0004U) | rd << 12;
            item.halves = {thumb_first(word), thumb_second(word)};
          } else {
            const std::uint32_t word = (store ? 0xf84b'0b04U : 0xf85b'0b04U) | rd << 12;
            item.halves = {thumb_first(word), thumb_second(word)};
          }
        } else if (pick < 8) {
          const unsigned kind = random.below(3);
          const unsigned q = 8 + random.below(4), q2 = 8 + random.below(4);
          std::uint32_t word = kind == 0   ? neon_vadd_i32(q, q2, 8 + random.below(4))
                               : kind == 1 ? neon_vld1_q(q, 10)
                                           : neon_vst1_q(q, 10);
          if (thumb)
            word = to_thumb_neon(word);
          item.halves = {thumb_first(word), thumb_second(word)};
        } else if (!thumb) {
          static constexpr unsigned ops[] = {0, 1, 2, 3, 4, 12, 13, 14};
          const unsigned op = ops[random.below(std::size(ops))];
          const unsigned cond = random.below(6) == 0 ? random.below(15) : 14;
          const bool setflags = random.below(2) != 0;
          const std::uint32_t word = cond << 28 | 0x0200'0000U | op << 21 |
                                     (setflags ? 1U << 20 : 0U) |
                                     (op == 13U ? 0U : rn) << 16 | rd << 12 |
                                     random.below(256);
          item.halves = {thumb_first(word), thumb_second(word)};
        } else {
          const unsigned lo = random.below(8), lo2 = random.below(8),
                         lo3 = random.below(8);
          switch (random.below(6)) {
          case 0: item.halves = {static_cast<std::uint16_t>(0x2000U | lo << 8 | random.below(256))}; break;
          case 1: item.halves = {static_cast<std::uint16_t>(0x1c00U | random.below(8) << 6 | lo2 << 3 | lo)}; break;
          case 2: item.halves = {static_cast<std::uint16_t>(0x1a00U | lo3 << 6 | lo2 << 3 | lo)}; break;
          case 3: item.halves = {static_cast<std::uint16_t>(0x1800U | lo3 << 6 | lo2 << 3 | lo)}; break;
          case 4: item.halves = {static_cast<std::uint16_t>(0x2800U | lo << 8 | random.below(256))}; break;
          default: item.halves = {static_cast<std::uint16_t>(0x4040U | lo2 << 3 | lo)}; break;
          }
        }
        items.push_back(item);
      }
      // The counter and the back edge: SUBS r12,r12,#1 then BNE head; or
      // SUBS, BEQ out, then an unconditional B head.
      Item subs;
      if (!thumb)
        subs.halves = {thumb_first(0xe25c'c001U), thumb_second(0xe25c'c001U)};
      else
        subs.halves = {0xf1bc, 0x0c01};
      items.push_back(subs);
      const int out = static_cast<int>(items.size()) + (counted ? 1 : 2);
      Item back;
      back.kind = Item::Kind::back_branch;
      back.target = 0;
      back.cond = counted ? 1 : 14; // NE, or always
      back.halves.assign(thumb ? 1 : 2, 0);
      if (counted) {
        items.push_back(back);
      } else {
        Item leave;
        leave.kind = Item::Kind::cond_branch;
        leave.cond = 0; // EQ
        leave.target = out;
        leave.halves.assign(thumb ? 1 : 2, 0);
        items.push_back(leave);
        items.push_back(back);
      }
      Item svc;
      svc.halves = thumb ? std::vector<std::uint16_t>{0xdf00}
                         : std::vector<std::uint16_t>{thumb_first(0xef00'0000U),
                                                      thumb_second(0xef00'0000U)};
      items.push_back(svc);
      // Positions, in code units (words for A32, halfwords for T32).
      std::vector<std::size_t> position(items.size() + 1);
      std::size_t at = 0;
      for (std::size_t i = 0; i < items.size(); ++i) {
        position[i] = at;
        at += thumb ? items[i].halves.size() : 1;
      }
      position[items.size()] = at;
      // Branches that skip past the loop's end go to the instruction after
      // the back edge.
      std::vector<std::uint16_t> code;
      for (std::size_t i = 0; i < items.size(); ++i) {
        auto &item = items[i];
        if (item.kind != Item::Kind::plain) {
          const std::size_t target_item =
              std::min<std::size_t>(static_cast<std::size_t>(item.target),
                                    items.size() - 1);
          const int delta = static_cast<int>(position[target_item]) -
                            static_cast<int>(position[i]) - 2;
          if (!thumb) {
            const std::uint32_t word = item.cond << 28 | 0x0a00'0000U |
                                       (static_cast<std::uint32_t>(delta) & 0x00ff'ffffU);
            item.halves = {thumb_first(word), thumb_second(word)};
          } else if (item.cond < 14) {
            item.halves = {static_cast<std::uint16_t>(
                0xd000U | item.cond << 8 | (static_cast<std::uint32_t>(delta) & 0xffU))};
          } else {
            item.halves = {static_cast<std::uint16_t>(
                0xe000U | (static_cast<std::uint32_t>(delta) & 0x7ffU))};
          }
        }
        for (const auto half : item.halves)
          code.push_back(half);
      }
      for (std::size_t i = 0; i < code.size(); ++i) {
        // An A32 program stores each word's halves in memory order, low half
        // first; a T32 program stores its halfwords in order.
        (void)i;
      }
      if (!thumb) {
        for (std::size_t i = 0; i < items.size(); ++i) {
          const std::uint32_t word =
              static_cast<std::uint32_t>(items[i].halves[0]) << 16 | items[i].halves[1];
          fixture.code32(offset + position[i] * 4, {word});
        }
      } else {
        for (std::size_t i = 0; i < code.size(); ++i)
          fixture.code16(offset + i * 2, {code[i]});
      }
      for (unsigned round = 0; round < 5; ++round) {
        auto cpu = random_registers(random, thumb);
        cpu.pc = thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
                       : key::a32_key(code_base + static_cast<std::uint32_t>(offset));
        cpu.registers[10] = data_base;
        // The walking base starts close to the end of the data, so a long
        // run faults in an iteration, or well inside it.
        const bool walks_off = random.below(3) == 0;
        cpu.registers[11] = walks_off
                                ? data_base + 2 * guest_page - 4 * (1 + random.below(40))
                                : data_base + 1024;
        const bool wraps = random.below(4) == 0;
        cpu.registers[12] = wraps ? 400 + random.below(300) : 1 + random.below(30);
        for (auto &q : cpu.vectors)
          for (auto &b : q.bytes)
            b = static_cast<std::byte>(random.next());
        for (std::size_t i = 0; i < window; i += 4) {
          const auto value = static_cast<std::uint32_t>(random.next());
          fixture.data32(i, value);
          fixture.data32(1024 + i, value ^ 0x5555'5555U);
        }
        const auto saved_low = fixture.read_data(0, window);
        const auto saved_high = fixture.read_data(1024, window + 4096);
        const auto run = [&](bool stepped) {
          fixture.data_bytes(0, saved_low);
          fixture.data_bytes(1024, saved_high);
          const auto outcome = stepped ? run_stepped_long(fixture, cpu)
                                       : run_whole_long(fixture, cpu);
          return std::pair{outcome, std::pair{fixture.read_data(0, window),
                                              fixture.read_data(1024, window + 4096)}};
        };
        const auto whole = run(false);
        const auto stepped = run(true);
        if (whole.first.reason != ExitReason::system_call &&
            whole.first.reason != ExitReason::memory_fault)
          std::fprintf(stderr, "%s loop %u round %u: exit %d retired %zu pc %llx\n",
                       thumb ? "t32" : "a32", p, round, static_cast<int>(whole.first.reason),
                       whole.first.retired,
                       static_cast<unsigned long long>(whole.first.cpu.pc));
        require(whole.first.reason == ExitReason::system_call ||
                whole.first.reason == ExitReason::memory_fault);
        require_same(whole.first, stepped.first, thumb ? "t32 loop" : "a32 loop", p);
        require(whole.second.first == stepped.second.first);
        require(whole.second.second == stepped.second.second);
        for (unsigned v = 8; v < 12; ++v)
          require(std::memcmp(&whole.first.cpu.vectors[v], &stepped.first.cpu.vectors[v],
                              sizeof whole.first.cpu.vectors[v]) == 0);
      }
    }
  }
}

/* A loop of Neon floating-point operations with nothing in its body that
 * needs the guest's FPCR runs them under the Standard value, which it sets
 * once before the head, instead of guarding every operation of every lap. It
 * must end as the same operations one at a time (each guarded, where the mode
 * is not known) do, on operands that are denormal, NaN or infinite and under
 * each rounding mode and flush-to-zero setting, flags included. */
void neon_float_loop_is_unguarded() {
  Fixture fixture{0x1a32'0c22, 4};
  // head: vld1.32 {d16,d17},[r10]; vmul.f32 q9,q8,q8; vadd.f32 q10,q10,q9;
  // add r10,r10,#16; subs r12,r12,#1; bne head; svc
  fixture.code32(0, {0xf46a'0a8fU, 0xf340'2df0U, 0xf244'4de2U, 0xe28a'a010U,
                     0xe25c'c001U, 0x1aff'fff9U, 0xef00'0000U});
  std::vector<std::byte> bytes(7 * 4);
  fixture.read_code(0, bytes);
  const auto compiled = ::uml::cpu::arm64::jit::testing::compile_a32_block(
      key::a32_key(code_base), bytes);
  require(compiled.code.guest_instructions == 6);
  require(has_host_loop(compiled.code.words, 6));
  // One write of the FPCR (msr fpcr, x0), in the preheader: no guard has a
  // cold path that writes it again.
  unsigned fpcr_writes = 0;
  for (const auto w : compiled.code.words)
    if (w == 0xd51b'4400U)
      ++fpcr_writes;
  require(fpcr_writes == 1);

  // The same loop with a VFP instruction in the body keeps its guards: the
  // body needs the guest's FPCR, so the Standard value is not set for it.
  {
    fixture.code32(256, {0xf46a'0a8fU, 0xf340'2df0U, 0xee30'0a81U, 0xf244'4de2U,
                         0xe28a'a010U, 0xe25c'c001U, 0x1aff'fff8U, 0xef00'0000U});
    std::vector<std::byte> mixed(8 * 4);
    fixture.read_code(256, mixed);
    const auto compiled_mixed = ::uml::cpu::arm64::jit::testing::compile_a32_block(
        key::a32_key(code_base + 256), mixed);
    require(compiled_mixed.code.guest_instructions == 7);
    unsigned writes = 0;
    for (const auto w : compiled_mixed.code.words)
      if (w == 0xd51b'4400U)
        ++writes;
    require(writes > 1);
  }

  static constexpr std::uint32_t pool[] = {
      0x3f80'0000U, 0x4049'0fdbU, 0x0000'0001U, 0x007f'ffffU, 0x8000'0001U,
      0x7fc0'0000U, 0x7f80'0001U, 0x7f80'0000U, 0xff80'0000U, 0x0080'0000U,
      0x7f7f'ffffU, 0x3380'0000U, 0x1f80'0000U, 0x0000'0000U, 0x8000'0000U,
      0x3eff'ffffU, 0x4b80'0000U, 0x1fff'ffffU};
  Lcg random{0x4e0a'f10dU};
  for (const std::size_t program : {std::size_t{0}, std::size_t{256}})
    for (const std::uint32_t fpcr : {0U, 3U << 22, 1U << 24, 1U << 25}) {
      for (unsigned round = 0; round < 24; ++round) {
        auto cpu = Fixture::task(false, static_cast<std::uint32_t>(program));
        cpu.registers[10] = data_base;
        cpu.registers[12] = 1 + random.below(16);
        cpu.registers[0] = 0;
        cpu.fpcr = fpcr;
        const bool special = round % 3 != 0;
        for (unsigned i = 0; i < 64; ++i)
          fixture.data32(i * 4, special && random.below(3) != 0
                                    ? pool[random.below(std::size(pool))]
                                    : 0x3f00'0000U +
                                          (static_cast<std::uint32_t>(
                                               random.next()) & 0x00ff'ffffU));
        for (unsigned q = 0; q < 11; ++q)
          if (q == 0 || q >= 8)
            for (unsigned lane = 0; lane < 4; ++lane) {
              const std::uint32_t v =
                  special && random.below(4) == 0
                      ? pool[random.below(std::size(pool))]
                      : 0x3f00'0000U +
                            (static_cast<std::uint32_t>(random.next()) &
                             0x00ff'ffffU);
              std::memcpy(&cpu.vectors[q].bytes[lane * 4], &v, 4);
            }
        const auto whole = run_whole_long(fixture, cpu);
        const auto stepped = run_stepped_long(fixture, cpu);
        require(whole.reason == ExitReason::system_call);
        require_same(whole, stepped, "neon float loop", round);
        for (unsigned q = 0; q < 11; ++q)
          require(std::memcmp(&whole.cpu.vectors[q], &stepped.cpu.vectors[q],
                              sizeof whole.cpu.vectors[q]) == 0);
        require(whole.cpu.fpsr == stepped.cpu.fpsr);
        require(whole.cpu.fpcr == stepped.cpu.fpcr);
      }
    }
}

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  unmapped_entry();
  it_blocks_whole_stepped_and_entered_mid_block();
  single_step_over_interworking_branches();
  long_blocks_and_the_budget();
  static_exits_chain();
  native_environment_is_carried_into_a_chain();
  core_registers_stay_in_the_host();
  polymorphic_returns_stay_in_the_chain();
  indirect_targets_are_per_address_space();
  rewritten_t32_halfwords();
  rewritten_a32_words();
  side_exit_branches();
  tlb_probe_is_the_short_form();
  aligned_accesses_skip_the_crossing_test();
  single_transfers_index_by_the_delta();
  backward_branches_are_side_exits();
  backward_side_exit_programs();
  conversions_keep_the_guest_registers();
  registers_survive_budget_returns_in_a_chain();
  linked_blocks_keep_the_registers_in_the_host();
  counted_loop_is_a_host_loop();
  neon_float_loop_is_unguarded();
  native_loops();
  extensions_decode_only_where_the_host_has_them();
  std::puts("arm64-jit-a32-control: ok");
  return 0;
}
