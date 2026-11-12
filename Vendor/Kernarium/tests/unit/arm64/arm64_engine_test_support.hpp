// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/memory.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>

namespace uml::emulator::tests {

namespace arm64 = ::uml::cpu::arm64;
using ArmFeature = ::uml::cpu::ArmFeature;
using ArmFeatureSet = ::uml::cpu::ArmFeatureSet;

inline constexpr std::uint64_t engine_test_code_base = 0x4f00'0000;
inline constexpr std::uint64_t engine_test_data_base = 0x5100'0000;

class NativeRequirements final {
public:
  [[nodiscard]] static NativeRequirements baseline() noexcept {
    return NativeRequirements{{}};
  }

  [[nodiscard]] static NativeRequirements
  features(ArmFeature feature) noexcept {
    ArmFeatureSet required;
    required.set(static_cast<std::size_t>(feature));
    return NativeRequirements{required};
  }

  [[nodiscard]] static NativeRequirements
  features(std::initializer_list<ArmFeature> features) noexcept {
    ArmFeatureSet required;
    for (const auto feature : features)
      required.set(static_cast<std::size_t>(feature));
    return NativeRequirements{required};
  }

  [[nodiscard]] const ArmFeatureSet &features() const noexcept {
    return requirements_;
  }

private:
  explicit NativeRequirements(ArmFeatureSet requirements) noexcept
      : requirements_{requirements} {}

  ArmFeatureSet requirements_;
};

enum class ExpectedEvent {
  system_call,
  memory_fault,
  illegal_instruction,
  breakpoint,
  floating_point_trap,
  pc_alignment,
  instruction_budget,
  fatal,
};

struct MemoryFaultExpectation final {
  std::uint64_t address{};
  arm64::MemoryAccess access{arm64::MemoryAccess::read};
};

using ArchitectureValidator = void (*)(
    const arm64::CpuState &initial, const arm64::CpuState &expected_template,
    const arm64::CpuState &actual, std::string_view case_name,
    std::string_view engine, std::uint32_t instruction);

/*
 * Optional test-only hooks bracket one engine invocation after memory has
 * been restored. They let concurrency cases observe the exact mapped backing
 * without duplicating the gadget/JIT selection logic.
 */
using EngineExecutionHook = void (*)(void *context, std::string_view engine,
                                     unsigned long generation);

struct MemoryRegion final {
  std::uint64_t address{};
  std::span<const std::byte> initial;
  /* An empty expected span means that the initial bytes are unchanged. */
  std::span<const std::byte> expected;
  bool readable{true};
  bool writable{true};
  bool executable{};
  bool guest_code{};
  bool guest_writable{true};
};

struct InstructionCase final {
  std::string_view name;
  std::uint32_t instruction{};
  arm64::CpuState initial;
  arm64::CpuState expected;
  ExpectedEvent event{ExpectedEvent::system_call};
  /* Required: native eligibility must come from the instruction allocation. */
  NativeRequirements native;
  MemoryFaultExpectation fault{};
  bool compare_fault{};
  /* Required ESR-shaped cause metadata for floating_point_trap. */
  std::uint32_t syndrome{};
  bool invalid_encoding{};
  std::span<const MemoryRegion> memory;
  /*
   * Optional architecture-level validation for instructions whose permitted
   * result is not a single bit-exact value. Event, fault, code, and memory
   * checks remain mandatory. When absent, every architectural field is
   * compared with expected.
  */
  ArchitectureValidator validator{};
  void *execution_context{};
  EngineExecutionHook before_engine{};
  EngineExecutionHook after_engine{};
  /*
   * InstructionCase normally derives this exactly: a successful instruction
   * plus the harness terminal SVC retires two instructions, while a
   * non-retiring terminal retires none. Override only for an instruction
   * whose own terminal behavior makes that default inapplicable.
   */
  std::optional<std::size_t> expected_instructions{};
};

struct ProgramWord final {
  std::size_t offset{};
  std::uint32_t instruction{};
};

struct ProgramCase final {
  std::string_view name;
  std::span<const ProgramWord> code;
  std::size_t entry_offset{};
  arm64::CpuState initial;
  /* Unlike InstructionCase, ProgramCase expects an absolute final PC. */
  arm64::CpuState expected;
  ExpectedEvent event{ExpectedEvent::system_call};
  /* Required: derived from every instruction that may execute. */
  NativeRequirements native;
  MemoryFaultExpectation fault{};
  bool compare_fault{};
  /* Required ESR-shaped cause metadata for floating_point_trap. */
  std::uint32_t syndrome{};
  bool invalid_encoding{};
  std::span<const MemoryRegion> memory;
  ArchitectureValidator validator{};
  void *execution_context{};
  EngineExecutionHook before_engine{};
  EngineExecutionHook after_engine{};
  /*
   * When supplied, every engine is checked against this specification value.
   * Otherwise the JIT must retire the gadget engine's count, except for
   * instruction-budget terminals whose engine-specific block-link fairness
   * accounting deliberately uses different boundary quanta.
   */
  std::optional<std::size_t> expected_instructions{};
  /*
   * The program holds a retry loop whose passes depend on the host: a
   * store-exclusive may fail spuriously, so how many instructions retire
   * is not a property of the program. No count is compared; the terminal
   * event, which a loop that never completes cannot reach inside the
   * budget, and the final state still are.
   */
  bool variable_instruction_count{};
};

struct EngineTestCounts final {
  std::size_t gadgets{};
  std::size_t jit{};
  std::size_t jit_skipped{};
  std::size_t jit_skipped_host_feature{};
  std::size_t jit_unavailable{};
};

[[nodiscard]] bool host_supports(NativeRequirements requirements) noexcept;

void require_architecture_equal(
    const arm64::CpuState &expected, const arm64::CpuState &actual,
    std::string_view case_name, std::string_view engine,
    std::uint32_t instruction,
    const std::source_location location = std::source_location::current());

/*
 * Runs each case through the production gadget engine and, when the host
 * implements the case's native features or the case is an invalid encoding,
 * the JIT. Every result must match the case's expected architectural state,
 * terminal event, memory, and retired count. The instruction corpus
 * (tests/unit/arm64/corpus/README.md) owns ordinary A64 behavior; this harness
 * serves focused tests that need engine hooks or host-private entry state.
 */
class EngineHarness final {
public:
  EngineHarness();
  ~EngineHarness();

  EngineHarness(const EngineHarness &) = delete;
  EngineHarness &operator=(const EngineHarness &) = delete;

  void check(const InstructionCase &test_case,
             const std::source_location location =
                 std::source_location::current());
  void check_program(const ProgramCase &test_case,
                     const std::source_location location =
                         std::source_location::current());

  void check_one(std::string_view name, std::uint32_t instruction,
                 const arm64::CpuState &initial,
                 const arm64::CpuState &expected, NativeRequirements native,
                 const std::source_location location =
                     std::source_location::current());

  void
  check_illegal(std::string_view name, std::uint32_t instruction,
                const arm64::CpuState &initial,
                NativeRequirements native = NativeRequirements::baseline(),
                const std::source_location location =
                    std::source_location::current());

  [[nodiscard]] const EngineTestCounts &counts() const noexcept;
  void print_summary(std::string_view suite) const;

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::emulator::tests
