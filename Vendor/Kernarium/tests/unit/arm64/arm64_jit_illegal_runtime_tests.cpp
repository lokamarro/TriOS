// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

using namespace uml::emulator::tests;

namespace {

namespace jit_test = uml::cpu::arm64::jit::testing;
using ::uml::cpu::EngineEvent;

constexpr std::uint32_t allocated_tag_add = 0x9180'0020U;
constexpr std::uint32_t guarded_pop = 0xd52b'7720U;
constexpr std::uint32_t set_dit = 0xd503'415fU;
constexpr std::uint32_t unknown_encoding = 0xffff'ffffU;
constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

void require_guest_illegal_compilation(std::uint64_t pc,
                                       std::uint32_t instruction) {
  const std::array code{instruction};
  const auto compiled = jit_test::compile_block(pc, code);
  require(compiled.guest_illegal);
  require(compiled.guest_instructions == 1);
  require(!compiled.words.empty());
}

/* An UNDEFINED encoding of an absent feature compiles to the same illegal
 * terminal as an unallocated word. */
void require_undefined_compilation(std::uint64_t pc,
                                   std::uint32_t instruction) {
  const std::array code{instruction};
  const std::array unknown{unknown_encoding};
  const auto compiled = jit_test::compile_block(pc, code);
  require(compiled.guest_instructions == 1);
  require(compiled.words == jit_test::compile_block(pc, unknown).words);
}

void first_word_illegal(unsigned long generation, std::uint64_t base,
                        std::uint32_t instruction,
                        uml::cpu::arm64::JitExecutorOptions options = {}) {
  JitBacking backing{1};
  backing.put32(0, instruction);
  backing.put32(4, terminal_system_call);
  JitMapping mapping{generation, base, backing, 1};

  uml::cpu::arm64::JitExecutor executor{options};
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = 0x1122'3344'5566'7788ULL;
  cpu.registers[1] = 0x8877'6655'4433'2211ULL;
  const auto initial = cpu;

  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base);
  require(cpu.registers == initial.registers);

  cpu.pc = base;
  executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base);
  require(cpu.registers == initial.registers);
  executor.end_execution();
}

void valid_host_unavailable_is_guest_illegal() {
  // MSR DIT translates only when the host implements FEAT_DIT.
  constexpr std::uint64_t base = 0x4fa0'0000;
  const std::array code{set_dit};
  const auto compiled = jit_test::compile_block_with_feature_override(
      base, code, ::uml::cpu::ArmFeature::dit, false);
  require(compiled.guest_illegal);
  require(compiled.guest_instructions == 1);
  uml::cpu::arm64::JitExecutorOptions options{};
  options.host_capabilities.supported.reset(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::dit));
  first_word_illegal(851, base, set_dit, options);
}

void valid_contract_unavailable_is_guest_illegal() {
  // The virtual CPU omits FEAT_MTE and FEAT_GCS.
  constexpr std::uint64_t base = 0x4fb0'0000;
  require_undefined_compilation(base, allocated_tag_add);
  first_word_illegal(852, base, allocated_tag_add);
  constexpr std::uint64_t guarded_base = 0x4fb1'0000;
  require_undefined_compilation(guarded_base, guarded_pop);
  first_word_illegal(855, guarded_base, guarded_pop);
  // Nor does it implement FEAT_SSBS, FEAT_HBC, FEAT_CMPBR, FEAT_XS, or
  // FEAT_WFxT, or FEAT_SME without the host's SME profile.
  if (!uml::cpu::arm64::implements_sme(uml::cpu::host_capabilities()))
    require_undefined_compilation(guarded_base + 0x1000,
                                  0xd503'477fU); // smstart
  for (const std::uint32_t instruction : {
           0xd503'413fU, // msr ssbs, #1
           0x5400'0030U, // bc.eq .+4
           0x7400'0022U, // cbgt w2, w0, .+4
           0xd503'323fU, // dsb oshnxs
           0xd503'1003U, // wfet x3
           0xd503'103eU, // wfit x30
       })
    require_undefined_compilation(guarded_base + 0x1000, instruction);
}

void unknown_word_is_guest_illegal() {
  constexpr std::uint64_t base = 0x4fc0'0000;
  require_guest_illegal_compilation(base, unknown_encoding);
  first_word_illegal(853, base, unknown_encoding);
}

void supported_prefix_stops_before_illegal_word() {
  constexpr unsigned long generation = 854;
  constexpr std::uint64_t base = 0x4fd0'0000;
  constexpr std::array code{
      add_x5_one,
      unknown_encoding,
      terminal_system_call,
  };
  const auto compiled = jit_test::compile_block(base, code);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == 1);

  JitBacking backing{1};
  for (std::size_t index = 0; index < code.size(); ++index)
    backing.put32(index * sizeof(std::uint32_t), code[index]);
  JitMapping mapping{generation, base, backing, 1};

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = 0xdead'beef'feed'faceULL;
  cpu.registers[1] = 0x0123'4567'89ab'cdefULL;
  cpu.registers[5] = 41;

  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1);
  require(cpu.pc == base + sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  require(cpu.registers[0] == 0xdead'beef'feed'faceULL);

  executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base + sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  require(cpu.registers[0] == 0xdead'beef'feed'faceULL);
  executor.end_execution();
}

void unrouted_semantic_word_is_fatal() {
  // Each semantic helper executes only the classes generated code routes to
  // it. Any other word is an internal failure that retires nothing and leaves
  // guest state untouched; it is never interpreted.
  namespace arm64 = uml::cpu::arm64;
  using Helper = std::uint64_t (*)(arm64::CpuState *, uml::memory::Tlb *,
                                   std::uint32_t) noexcept;
  constexpr std::uint32_t add_x0_x1_x2 = 0x8b02'0020U;
  constexpr std::uint32_t store_x0_x1 = 0xf900'0020U;
  struct Case final {
    Helper helper;
    std::uint32_t instruction;
  };
  const std::array cases{
      Case{&arm64::uml_arm64_jit_execute_memory, add_x0_x1_x2},
      Case{&arm64::uml_arm64_jit_execute_exclusive, store_x0_x1},
      Case{&arm64::uml_arm64_jit_execute_ordered, store_x0_x1},
  };
  for (const auto &test_case : cases) {
    arm64::CpuState cpu{};
    cpu.pc = 0x4fe0'0000U;
    cpu.registers[0] = 0x1122'3344'5566'7788ULL;
    cpu.registers[1] = 0x4fe0'1000U;
    cpu.registers[2] = 3;
    const auto initial = cpu;
    uml::memory::Tlb tlb;
    const auto result =
        test_case.helper(&cpu, &tlb, test_case.instruction);
    require(static_cast<std::uint32_t>(result) ==
            static_cast<std::uint32_t>(EngineEvent::fatal));
    require((result >> 32U) == 0U);
    require(cpu.pc == initial.pc);
    require(cpu.registers == initial.registers);
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  valid_host_unavailable_is_guest_illegal();
  valid_contract_unavailable_is_guest_illegal();
  unknown_word_is_guest_illegal();
  supported_prefix_stops_before_illegal_word();
  unrouted_semantic_word_is_fatal();
  return 0;
}
