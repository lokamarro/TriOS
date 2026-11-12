// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <array>
#include <cstdint>

using namespace uml::emulator::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t standard(std::uint32_t base,
                                               unsigned option) noexcept {
  return base | ((option & 15U) << 8U);
}

void current_host_barriers_execute() {
  constexpr unsigned long generation = 841;
  constexpr std::uint64_t base = 0x4f90'0000;
  constexpr std::uint32_t dsb_base = 0xd503'309fU;
  constexpr std::uint32_t dmb_base = 0xd503'30bfU;
  constexpr std::uint32_t isb_base = 0xd503'30dfU;
  constexpr std::array code{
      standard(dmb_base, 3),
      standard(dsb_base, 15),
      standard(isb_base, 15),
      std::uint32_t{0xd503'30ffU},
      std::uint32_t{0xd400'0001U},
  };

  JitBacking backing{1};
  for (std::size_t index = 0; index < code.size(); ++index)
    backing.put32(index * sizeof(std::uint32_t), code[index]);
  JitMapping mapping{generation, base, backing, 1};

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
    cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^ (std::uint64_t{reg} << 32U);
  const auto initial_registers = cpu.registers;
  cpu.pc = base;

  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::system_call);
  require(executed == code.size());
  require(cpu.pc == base + code.size() * sizeof(std::uint32_t));
  require(cpu.registers == initial_registers);
  executor.end_execution();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  current_host_barriers_execute();
  return 0;
}
