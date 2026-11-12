// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's AArch32 entry end to end: a first A32 and T32 program, the exits
 * and their PC keys, and the retired counts. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

void a32_arithmetic_and_svc() {
  Fixture fixture{0x1a32'0001};
  fixture.code32(0, {
      0xe3a00005U, // mov r0, #5
      0xe0801100U, // add r1, r0, r0, lsl #2
      0xe29124ffU, // adds r2, r1, #0xff000000
      0xef000000U, // svc #0
  });
  auto cpu = Fixture::task(false);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require(cpu.registers[0] == 5);
  require(cpu.registers[1] == 25);
  require(cpu.registers[2] == 0xff00'0019U);
  require((cpu.nzcv & 0xf000'0000U) == flag_n);
  require(cpu.pc == key::a32_key(code_base + 16));
}

void t32_arithmetic_and_svc() {
  Fixture fixture{0x1a32'0002};
  fixture.code16(0, {
      0x2005, // movs r0, #5
      0x1801, // adds r1, r0, r0
      0x1dca, // adds r2, r1, #7
      0xdf00, // svc #0
  });
  auto cpu = Fixture::task(true);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require(cpu.registers[0] == 5);
  require(cpu.registers[1] == 10);
  require(cpu.registers[2] == 17);
  require((cpu.nzcv & 0xf000'0000U) == 0);
  require(cpu.pc == key::t32_key(code_base + 8));
}

void bkpt_reports_its_immediate() {
  Fixture fixture{0x1a32'0003};
  fixture.code32(0, {
      0xe3a00001U, // mov r0, #1
      0xe1212374U, // bkpt #0x1234
  });
  auto cpu = Fixture::task(false);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::breakpoint);
  require(result.syndrome == 0x1234U);
  require(cpu.pc == key::a32_key(code_base + 4));
  require(cpu.registers[0] == 1);
}

void t32_bkpt_reports_its_immediate() {
  Fixture fixture{0x1a32'0004};
  fixture.code16(0, {
      0x2001, // movs r0, #1
      0xbe42, // bkpt #0x42
  });
  auto cpu = Fixture::task(true);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::breakpoint);
  require(result.syndrome == 0x42U);
  require(cpu.pc == key::t32_key(code_base + 2));
  require(cpu.registers[0] == 1);
}

/* The PC key is checked at every block boundary: an A32 target with bit 1 set
 * is a PC-alignment exit at that PC, a PC that is no AArch32 key is fatal,
 * and a key with bits outside its 32-bit address is a PC-alignment exit at
 * the PC its low bits name. */
void a_misaligned_or_foreign_pc_key_ends_the_entry() {
  {
    Fixture fixture{0x1a32'0005};
    fixture.code32(0, {
        0xe3a01002U, // mov r1, #2
        0xe12fff10U, // bx r0
    });
    auto cpu = Fixture::task(false);
    cpu.registers[0] = code_base + 0x102;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::pc_alignment);
    require(result.retired_instructions == 2);
    require(result.fault.address == code_base + 0x102);
    require(result.fault.access == ::uml::memory::MemoryAccess::execute);
    require(cpu.registers[1] == 2);
  }
  {
    Fixture fixture{0x1a32'0006};
    auto cpu = Fixture::task(false);
    cpu.pc = code_base;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::fatal);
    require(result.retired_instructions == 0);
  }
  {
    Fixture fixture{0x1a32'0007};
    auto cpu = Fixture::task(false);
    cpu.pc = key::a32_key(code_base) | std::uint64_t{1} << 40;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::pc_alignment);
    require(result.retired_instructions == 0);
    require(result.fault.address == code_base);
    require(result.fault.access == ::uml::memory::MemoryAccess::execute);
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  if (std::getenv("A32_TEST_DUMP") != nullptr) {
    const std::array<std::uint32_t, 4> words{0xe3a00005U, 0xe0801100U,
                                             0xe29124ffU, 0xef000000U};
    dump_translation(key::a32_key(code_base), std::as_bytes(std::span{words}));
    return 0;
  }
  a32_arithmetic_and_svc();
  t32_arithmetic_and_svc();
  bkpt_reports_its_immediate();
  t32_bkpt_reports_its_immediate();
  a_misaligned_or_foreign_pc_key_ends_the_entry();
  std::puts("arm64-jit-a32-runtime: ok");
  return 0;
}
