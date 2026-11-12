// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

namespace {

constexpr std::size_t transfer_offset = 64;
constexpr std::uint64_t transfer_value = 0xf1e2'd3c4'b5a6'9788ULL;
std::array<std::string_view, 2> validator_order{};
std::size_t validator_calls{};

struct HookTrace final {
  std::array<std::string_view, 4> engines{};
  std::array<bool, 4> before{};
  std::size_t calls{};
  bool active{};
  std::string_view active_engine{};
  unsigned long generation{};
};

void reset(HookTrace &trace) { trace = {}; }

void before_engine(void *opaque, std::string_view engine,
                   unsigned long generation) {
  auto &trace = *static_cast<HookTrace *>(opaque);
  if (trace.active || trace.calls >= trace.engines.size())
    std::abort();
  if (trace.calls == 0)
    trace.generation = generation;
  else if (trace.generation != generation)
    std::abort();
  trace.engines[trace.calls] = engine;
  trace.before[trace.calls++] = true;
  trace.active = true;
  trace.active_engine = engine;
}

void after_engine(void *opaque, std::string_view engine,
                  unsigned long generation) {
  auto &trace = *static_cast<HookTrace *>(opaque);
  if (!trace.active || trace.active_engine != engine ||
      trace.generation != generation || trace.calls >= trace.engines.size())
    std::abort();
  trace.engines[trace.calls] = engine;
  trace.before[trace.calls++] = false;
  trace.active = false;
  trace.active_engine = {};
}

void require_hook_order(const HookTrace &trace,
                        std::span<const std::string_view> engines) {
  if (trace.active || trace.calls != engines.size() * 2)
    std::abort();
  for (std::size_t index = 0; index < engines.size(); ++index) {
    const auto offset = index * 2;
    if (trace.engines[offset] != engines[index] ||
        trace.engines[offset + 1] != engines[index] ||
        !trace.before[offset] || trace.before[offset + 1])
      std::abort();
  }
}

[[nodiscard]] constexpr std::uint32_t load_store(bool load, unsigned immediate,
                                                 unsigned base,
                                                 unsigned target) noexcept {
  return 0xf900'0000U | (load ? 0x0040'0000U : 0U) |
         ((immediate & 0xfffU) << 10) | ((base & 31U) << 5) | (target & 31U);
}

[[nodiscard]] arm64::CpuState initial_state() {
  arm64::CpuState cpu{};
  for (std::size_t index = 0; index < cpu.registers.size(); ++index)
    cpu.registers[index] =
        0x1020'3040'5060'7080ULL ^ (index * 0x0101'0101'0101'0101ULL);
  cpu.registers[2] = engine_tests::engine_test_data_base;
  cpu.nzcv = 0xa000'0000U;
  return cpu;
}

void record_validator_engine(std::string_view engine) {
  if (validator_calls >= validator_order.size())
    std::abort();
  validator_order[validator_calls++] = engine;
}

void validate_increment(const arm64::CpuState &initial,
                        const arm64::CpuState &expected_template,
                        const arm64::CpuState &actual,
                        std::string_view case_name, std::string_view engine,
                        std::uint32_t instruction) {
  record_validator_engine(engine);
  auto expected = initial;
  expected.registers[0] += 1;
  expected.pc = expected_template.pc;
  engine_tests::require_architecture_equal(expected, actual, case_name, engine,
                                           instruction);
}

void validate_absolute(const arm64::CpuState &initial,
                       const arm64::CpuState &expected_template,
                       const arm64::CpuState &actual,
                       std::string_view case_name, std::string_view engine,
                       std::uint32_t instruction) {
  record_validator_engine(engine);
  auto expected = initial;
  const auto source = initial.registers[1];
  expected.registers[0] =
      static_cast<std::int64_t>(source) < 0 ? 0U - source : source;
  expected.pc = expected_template.pc;
  engine_tests::require_architecture_equal(expected, actual, case_name, engine,
                                           instruction);
}

void load_preserves_memory(engine_tests::EngineHarness &harness) {
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
  std::memcpy(page.data() + transfer_offset, &transfer_value,
              sizeof(transfer_value));
  const engine_tests::MemoryRegion region{
      .address = engine_tests::engine_test_data_base,
      .initial = std::span<const std::byte>{page},
  };
  const auto initial = initial_state();
  auto expected = initial;
  expected.registers[3] = transfer_value;
  harness.check({
      .name = "engine-harness-load",
      .instruction = load_store(true, transfer_offset / 8, 2, 3),
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .memory = std::span<const engine_tests::MemoryRegion>{&region, 1},
  });
}

void store_updates_memory(engine_tests::EngineHarness &harness) {
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_page{};
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> expected_page{};
  expected_page = initial_page;
  std::memcpy(expected_page.data() + transfer_offset, &transfer_value,
              sizeof(transfer_value));
  const engine_tests::MemoryRegion region{
      .address = engine_tests::engine_test_data_base,
      .initial = std::span<const std::byte>{initial_page},
      .expected = std::span<const std::byte>{expected_page},
  };
  auto initial = initial_state();
  initial.registers[3] = transfer_value;
  harness.check({
      .name = "engine-harness-store",
      .instruction = load_store(false, transfer_offset / 8, 2, 3),
      .initial = initial,
      .expected = initial,
      .native = engine_tests::NativeRequirements::baseline(),
      .memory = std::span<const engine_tests::MemoryRegion>{&region, 1},
  });
}

void fault_preserves_state_and_memory(engine_tests::EngineHarness &harness) {
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
  const engine_tests::MemoryRegion region{
      .address = engine_tests::engine_test_data_base,
      .initial = std::span<const std::byte>{page},
      .readable = true,
      .writable = false,
      .guest_writable = false,
  };
  auto initial = initial_state();
  initial.registers[3] = transfer_value;
  harness.check({
      .name = "engine-harness-store-fault",
      .instruction = load_store(false, transfer_offset / 8, 2, 3),
      .initial = initial,
      .expected = initial,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::baseline(),
      .fault =
          {
              .address = engine_tests::engine_test_data_base + transfer_offset,
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
      .memory = std::span<const engine_tests::MemoryRegion>{&region, 1},
  });
}

void code_page_is_immutable(engine_tests::EngineHarness &harness) {
  auto initial = initial_state();
  initial.registers[2] = engine_tests::engine_test_code_base;
  initial.registers[3] = transfer_value;
  harness.check({
      .name = "engine-harness-code-page-is-immutable",
      .instruction = load_store(false, 0, 2, 3),
      .initial = initial,
      .expected = initial,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::baseline(),
      .fault =
          {
              .address = engine_tests::engine_test_code_base,
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
  });
}

void zero_address_fault_is_recorded(engine_tests::EngineHarness &harness) {
  auto initial = initial_state();
  initial.registers[2] = 0;
  harness.check({
      .name = "engine-harness-zero-address-fault",
      .instruction = load_store(true, 0, 2, 3),
      .initial = initial,
      .expected = initial,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::baseline(),
      .fault =
          {
              .address = 0,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
  });
}

void memory_fault_keeps_exact_page_address(
    engine_tests::EngineHarness &harness) {
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
  const engine_tests::MemoryRegion region{
      .address = engine_tests::engine_test_data_base,
      .initial = std::span<const std::byte>{page},
  };
  auto initial = initial_state();
  initial.registers[2] = engine_tests::engine_test_data_base +
                         DARWIN_UML_GUEST_PAGE_SIZE - 4;
  harness.check({
      .name = "engine-harness-cross-page-fault-address",
      .instruction = load_store(true, 0, 2, 3),
      .initial = initial,
      .expected = initial,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::baseline(),
      .fault =
          {
              .address = engine_tests::engine_test_data_base +
                         DARWIN_UML_GUEST_PAGE_SIZE,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .memory = std::span<const engine_tests::MemoryRegion>{&region, 1},
  });
}

void program_layout_and_control_flow(engine_tests::EngineHarness &harness) {
  constexpr std::size_t entry_offset = 16;
  const std::array arithmetic_program{
      engine_tests::ProgramWord{.offset = entry_offset,
                                .instruction = 0x9100'0400U},
      engine_tests::ProgramWord{.offset = entry_offset + 4,
                                .instruction = 0x9100'0400U},
      engine_tests::ProgramWord{.offset = entry_offset + 8,
                                .instruction = 0xd400'0001U},
  };
  auto initial = initial_state();
  initial.registers[0] = 10;
  auto expected = initial;
  expected.registers[0] = 12;
  expected.pc = engine_tests::engine_test_code_base + entry_offset +
                3 * sizeof(std::uint32_t);
  harness.check_program({
      .name = "engine-harness-program",
      .code = std::span<const engine_tests::ProgramWord>{arithmetic_program},
      .entry_offset = entry_offset,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = 3,
  });

  const std::array branch_program{
      engine_tests::ProgramWord{.offset = 0, .instruction = 0x1400'0002U},
      engine_tests::ProgramWord{.offset = 4, .instruction = 0x0000'0000U},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  initial = initial_state();
  expected = initial;
  expected.pc = engine_tests::engine_test_code_base + 12;
  harness.check_program({
      .name = "engine-harness-control-flow",
      .code = std::span<const engine_tests::ProgramWord>{branch_program},
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = 2,
  });
}

void direct_system_call_has_one_retirement_and_one_word_pc_advance(
    engine_tests::EngineHarness &harness) {
  const auto initial = initial_state();
  harness.check({
      .name = "engine-harness-direct-system-call",
      .instruction = 0xd400'0001U,
      .initial = initial,
      .expected = initial,
      .native = engine_tests::NativeRequirements::baseline(),
  });
}

void self_loop_reports_instruction_budget(
    engine_tests::EngineHarness &harness) {
  const std::array loop{
      engine_tests::ProgramWord{.offset = 0, .instruction = 0x1400'0000U},
  };
  const auto initial = initial_state();
  auto expected = initial;
  expected.pc = engine_tests::engine_test_code_base;
  harness.check_program({
      .name = "engine-harness-self-loop-budget",
      .code = std::span<const engine_tests::ProgramWord>{loop},
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::instruction_budget,
      .native = engine_tests::NativeRequirements::baseline(),
  });
}

void single_instruction_case_can_report_instruction_budget(
    engine_tests::EngineHarness &harness) {
  const auto initial = initial_state();
  harness.check({
      .name = "engine-harness-single-instruction-loop-budget",
      .instruction = 0x1400'0000U,
      .initial = initial,
      .expected = initial,
      .event = engine_tests::ExpectedEvent::instruction_budget,
      .native = engine_tests::NativeRequirements::baseline(),
  });
}

void invalid_encoding_bypasses_native_feature_gate(
    engine_tests::EngineHarness &harness) {
  const auto before = harness.counts();
  harness.check_illegal(
      "engine-harness-invalid-bypasses-feature-gate",
      0x0e60'e000U, initial_state(),
      engine_tests::NativeRequirements::features(
          {::uml::cpu::ArmFeature::advanced_simd,
           ::uml::cpu::ArmFeature::rme_gpc3}));
  const auto &after = harness.counts();
  if (after.gadgets != before.gadgets + 1 ||
      after.jit != before.jit + 1 ||
      after.jit_skipped != before.jit_skipped ||
      after.jit_skipped_host_feature != before.jit_skipped_host_feature ||
      after.jit_unavailable != before.jit_unavailable)
    std::abort();
}

void validators_follow_engine_order_and_feature_skips(
    engine_tests::EngineHarness &harness) {
  constexpr auto increment = 0x9100'0400U;
  const auto initial = initial_state();
  auto deliberately_non_oracle = initial;
  deliberately_non_oracle.registers[0] = 0;
  HookTrace hooks;

  validator_calls = 0;
  const auto before_baseline = harness.counts();
  harness.check({
      .name = "engine-harness-validator-order",
      .instruction = increment,
      .initial = initial,
      .expected = deliberately_non_oracle,
      .native = engine_tests::NativeRequirements::baseline(),
      .validator = &validate_increment,
      .execution_context = &hooks,
      .before_engine = &before_engine,
      .after_engine = &after_engine,
  });
  const auto after_baseline = harness.counts();
  const bool jit_available =
      after_baseline.jit == before_baseline.jit + 1;
  if (after_baseline.gadgets != before_baseline.gadgets + 1 ||
      validator_calls != (jit_available ? 2U : 1U) ||
      validator_order[0] != "gadgets" ||
      (jit_available && validator_order[1] != "jit"))
    std::abort();
  constexpr std::array<std::string_view, 2> baseline_engines{"gadgets",
                                                             "jit"};
  require_hook_order(hooks,
                     std::span<const std::string_view>{baseline_engines}.first(
                         jit_available ? 2U : 1U));

  const auto unavailable = engine_tests::NativeRequirements::features(
      ::uml::cpu::ArmFeature::cssc);
  if (engine_tests::host_supports(unavailable))
    return;
  validator_calls = 0;
  reset(hooks);
  const auto before_skip = harness.counts();
  harness.check({
      .name = "engine-harness-validator-feature-skip",
      .instruction = 0xdac0'2020U, // ABS X0, X1
      .initial = initial,
      .expected = deliberately_non_oracle,
      .native = unavailable,
      .validator = &validate_absolute,
      .execution_context = &hooks,
      .before_engine = &before_engine,
      .after_engine = &after_engine,
  });
  const auto after_skip = harness.counts();
  if (after_skip.gadgets != before_skip.gadgets + 1 ||
      after_skip.jit != before_skip.jit || validator_calls != 1 ||
      validator_order[0] != "gadgets" ||
      (jit_available
           ? after_skip.jit_skipped_host_feature !=
                 before_skip.jit_skipped_host_feature + 1
           : after_skip.jit_unavailable != before_skip.jit_unavailable + 1))
    std::abort();
  constexpr std::array<std::string_view, 1> skipped_engines{"gadgets"};
  require_hook_order(hooks, skipped_engines);
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  load_preserves_memory(harness);
  store_updates_memory(harness);
  fault_preserves_state_and_memory(harness);
  code_page_is_immutable(harness);
  zero_address_fault_is_recorded(harness);
  memory_fault_keeps_exact_page_address(harness);
  program_layout_and_control_flow(harness);
  direct_system_call_has_one_retirement_and_one_word_pc_advance(harness);
  self_loop_reports_instruction_budget(harness);
  single_instruction_case_can_report_instruction_budget(harness);
  invalid_encoding_bypasses_native_feature_gate(harness);
  validators_follow_engine_order_and_feature_skips(harness);
  const auto &counts = harness.counts();
  const std::size_t expected_cases =
      engine_tests::host_supports(engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::cssc))
          ? 13
          : 14;
  if (counts.gadgets != expected_cases ||
      counts.jit + counts.jit_skipped != expected_cases)
    std::abort();
  harness.print_summary("arm64-engine-harness");
  return 0;
}
