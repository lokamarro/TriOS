// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/engine_factory.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

namespace arm64 = ::uml::cpu::arm64;
using ::uml::cpu::CpuEngine;
using ::uml::cpu::EngineKind;
using ::uml::cpu::ExecutionRequest;
using ::uml::cpu::ExitReason;
using ::uml::emulator::tests::JitBacking;
using ::uml::emulator::tests::JitMapping;

constexpr std::uint64_t code_base = 0x6e00'0000;
constexpr unsigned long generation = 0xc811;
constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t branch_forward_16 = 0x1400'0004U;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t breakpoint = 0xd420'0000U;
constexpr std::uint32_t fmov_d0_one = 0x1e6e'1000U;

[[noreturn]] void fail(const char *engine, const char *message) {
  std::fprintf(stderr, "arm64-single-step: %s: %s\n", engine, message);
  std::abort();
}

void require(bool condition, const char *engine, const char *message) {
  if (!condition)
    fail(engine, message);
}

struct Step final {
  ExitReason reason{ExitReason::fatal};
  std::size_t retired{};
};

/* One memory view per engine, as each runtime thread keeps. */
Step run(CpuEngine &engine, ::uml::memory::StaticGuestMemory &memory,
         arm64::CpuState &cpu, bool single_step) {
  ExecutionRequest request{
      .cpu = cpu,
      .memory = memory,
      .translated_memory = &memory,
      .generation = generation,
      .instruction_budget = 64,
      .single_step = single_step,
  };
  const auto result = engine.run(request);
  return {result.reason, result.retired_instructions};
}

/* A debugger steps straight-line code, a taken branch, and FP/SIMD code one
 * instruction at a time. A stepped SVC or BRK reports its own exit instead,
 * and the next unstepped entry runs whole blocks again. */
void steps_one_instruction(EngineKind kind, const char *name) {
  auto engine = ::uml::cpu::make_cpu_engine(kind);
  require(engine != nullptr, name, "engine was not created");
  auto memory = std::make_unique<::uml::memory::StaticGuestMemory>();
  arm64::CpuState cpu{};
  cpu.pc = code_base;

  auto step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::single_step && step.retired == 1 &&
              cpu.pc == code_base + 4 && cpu.registers[5] == 1,
          name, "first add did not step");
  step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::single_step && step.retired == 1 &&
              cpu.pc == code_base + 8 && cpu.registers[5] == 2,
          name, "second add did not step");
  step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::single_step && step.retired == 1 &&
              cpu.pc == code_base + 0x18,
          name, "taken branch did not step to its target");
  step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::single_step && step.retired == 1 &&
              cpu.pc == code_base + 0x1c,
          name, "FP instruction did not step");
  step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::system_call &&
              cpu.pc == code_base + 0x20,
          name, "stepped SVC did not report the system call");
  step = run(*engine, *memory, cpu, true);
  require(step.reason == ExitReason::breakpoint &&
              cpu.pc == code_base + 0x20,
          name, "stepped BRK did not report the breakpoint");

  cpu = {};
  cpu.pc = code_base;
  step = run(*engine, *memory, cpu, false);
  require(step.reason == ExitReason::system_call && step.retired == 5 &&
              cpu.registers[5] == 2,
          name, "an unstepped entry did not run to the system call");
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  JitBacking backing{1};
  backing.put32(0x00, add_x5_one);
  backing.put32(0x04, add_x5_one);
  backing.put32(0x08, branch_forward_16);
  backing.put32(0x18, fmov_d0_one);
  backing.put32(0x1c, system_call);
  backing.put32(0x20, breakpoint);
  JitMapping mapping{generation, code_base, backing, 1};

  steps_one_instruction(EngineKind::gadgets, "gadgets");
  if (::uml::cpu::cpu_engine_available(EngineKind::jit)) {
    const auto budget = arm64::minimum_jit_cache_budget(1);
    require(budget != 0 && arm64::set_jit_cache_budget(budget, 1), "jit",
            "could not publish the JIT cache budget");
    steps_one_instruction(EngineKind::jit, "jit");
  }
  return EXIT_SUCCESS;
}
