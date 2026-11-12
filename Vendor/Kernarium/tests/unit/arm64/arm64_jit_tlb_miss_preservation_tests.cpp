// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
simd_modified_immediate(unsigned immediate, unsigned destination) noexcept {
  return 0x4f00'0400U | ((immediate & 0xe0U) << 11U) | (14U << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
indexed_halfword_multiply(unsigned destination, unsigned left,
                          unsigned element) noexcept {
  return 0x4f40'8000U | ((element & 15U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

void apply_indexed_halfword_multiply(arm64::CpuState &cpu,
                                     unsigned reg) {
  const auto source = cpu.vectors[reg];
  for (unsigned lane_index = 0; lane_index < 8U; ++lane_index) {
    const auto offset = lane_index * 2U;
    const auto lane = static_cast<std::uint16_t>(
        std::to_integer<unsigned>(source.bytes[offset]) |
        (std::to_integer<unsigned>(source.bytes[offset + 1U]) << 8U));
    const auto element = static_cast<std::uint16_t>(
        std::to_integer<unsigned>(source.bytes[0]) |
        (std::to_integer<unsigned>(source.bytes[1]) << 8U));
    const auto result = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(lane) * element);
    cpu.vectors[reg].bytes[offset] = static_cast<std::byte>(result);
    cpu.vectors[reg].bytes[offset + 1U] =
        static_cast<std::byte>(result >> 8U);
  }
}

[[nodiscard]] constexpr std::uint32_t
load_x(unsigned destination, unsigned base) noexcept {
  return 0xf940'0000U | ((base & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
load_pair_x(unsigned first, unsigned second, unsigned base,
            std::size_t offset) noexcept {
  return 0xa940'0000U |
         ((static_cast<std::uint32_t>(offset / 8U) & 0x7fU) << 15U) |
         ((second & 31U) << 10U) | ((base & 31U) << 5U) | (first & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_x(unsigned destination, unsigned source) noexcept {
  return 0xaa00'03e0U | ((source & 31U) << 16U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
add_shifted(unsigned destination, unsigned left, unsigned right) noexcept {
  return 0x8b00'0000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr bool
is_compare_nonzero_x(std::uint32_t instruction, unsigned reg) noexcept {
  return (instruction & 0xff00'001fU) ==
         (0xb500'0000U | (reg & 31U));
}

[[nodiscard]] constexpr bool
is_compare_zero_x(std::uint32_t instruction, unsigned reg) noexcept {
  return (instruction & 0xff00'001fU) ==
         (0xb400'0000U | (reg & 31U));
}

[[nodiscard]] constexpr bool
is_unconditional_branch(std::uint32_t instruction) noexcept {
  return (instruction & 0xfc00'0000U) == 0x1400'0000U;
}

[[nodiscard]] constexpr std::ptrdiff_t
sign_extend_immediate(std::uint32_t value, unsigned bits) noexcept {
  const auto sign = std::uint32_t{1} << (bits - 1U);
  return static_cast<std::ptrdiff_t>(
      static_cast<std::int32_t>((value ^ sign) - sign));
}

[[nodiscard]] constexpr std::ptrdiff_t
compare_branch_target(std::size_t source,
                      std::uint32_t instruction) noexcept {
  return static_cast<std::ptrdiff_t>(source) +
         sign_extend_immediate((instruction >> 5U) & 0x7ffffU, 19U);
}

[[nodiscard]] constexpr std::ptrdiff_t
unconditional_branch_target(std::size_t source,
                            std::uint32_t instruction) noexcept {
  return static_cast<std::ptrdiff_t>(source) +
         sign_extend_immediate(instruction & 0x03ff'ffffU, 26U);
}

void miss_call_is_cold_and_has_no_resident_spills() {
  constexpr std::array guest{
      0xf900'0001U, // str x1, [x0]
  };
  const auto compiled =
      jit_test::compile_block(engine_tests::engine_test_code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the memory operation");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated code");

  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  const auto all = std::span<const std::uint32_t>{compiled.words};
  for (const auto instruction : hot)
    require(instruction != 0xd63f'0200U,
            "TLB miss helper remained in the hot-word prefix");

  constexpr std::uint32_t tag_difference = 0xca01'0042U;
  const auto translated_address = add_shifted(8, 8, 9);
  std::size_t probe = hot.size();
  for (std::size_t index = 0; index + 2U < hot.size(); ++index) {
    if (hot[index] != tag_difference ||
        !is_compare_nonzero_x(hot[index + 1U], 2) ||
        hot[index + 2U] != translated_address)
      continue;
    require(probe == hot.size(),
            "memory block emitted multiple candidate probes");
    probe = index;
  }
  require(probe != hot.size(),
          "memory block omitted the TLB probe");
  require(probe != 0U &&
              hot[probe - 1U] == load_pair_x(2, 9, 0, 16),
          "write translation loaded the wrong tag/delta fields");
  const auto continuation = probe + 3U;
  const auto cold_probe = compare_branch_target(
      probe + 1U, hot[probe + 1U]);
  require(cold_probe >= static_cast<std::ptrdiff_t>(compiled.hot_words) &&
              cold_probe < static_cast<std::ptrdiff_t>(all.size()),
          "TLB miss does not target deferred translation code");

  std::size_t call = all.size();
  for (std::size_t index = compiled.hot_words; index < all.size(); ++index) {
    if (all[index] != 0xd63f'0200U)
      continue;
    call = index;
    break;
  }
  require(call != all.size(), "memory block omitted the TLB miss helper");
  require(call >= compiled.hot_words,
          "TLB miss helper was counted as hot code");

  std::size_t arguments = call;
  while (arguments != compiled.hot_words &&
         all[arguments] != move_x(0, 14))
    --arguments;
  require(all[arguments] == move_x(0, 14),
          "TLB miss call omitted the preserved TLB context");
  require(arguments + 3 < call,
          "TLB miss call did not materialize its arguments");
  require(all[arguments + 1] == move_x(1, 8),
          "TLB miss call did not pass the guest address directly");
  require(all[arguments + 2] == move_wide(2, 1),
          "TLB miss call did not materialize write access in x2");
  auto call_setup = arguments + 3U;
  for (std::size_t index = call_setup; index < call; ++index)
    require((all[index] & 31U) == 16U,
            "TLB miss call contains an inline spill or reload");
  auto result = call + 1U;
  require(result + 2U < all.size() && all[result] == move_x(8, 0) &&
              is_compare_zero_x(all[result + 1U], 8),
          "TLB miss result was not consumed without a context reload");
  require(is_unconditional_branch(all[result + 2U]) &&
              unconditional_branch_target(
                  result + 2U, all[result + 2U]) ==
                  static_cast<std::ptrdiff_t>(continuation),
          "successful TLB miss does not rejoin the exact memory continuation");

  /*
   * Deferring the preserving miss bridge leaves a 24-word single-store hot
   * body; a translation compiled without top-byte ignore holds no tag work.
   * The persistent linked-block ABI contributes seven fixed prologue/epilogue
   * words.
   */
  const std::size_t persistent_hot_frame_words = 7U;
  const std::size_t persistent_total_frame_words = 13U;
  require(compiled.hot_words <= 27 + persistent_hot_frame_words,
          "single-store JIT hot expansion lost the "
          "deferred-translation budget");
  require(compiled.words.size() <= 60 + persistent_total_frame_words,
          "single-store JIT total expansion exceeded the "
          "preserving-call budget");
}

void every_resident_register_survives_a_forced_miss() {
  constexpr std::size_t gpr_count = 8;
  constexpr std::size_t vector_count = 18;
  constexpr std::size_t load_index = gpr_count + vector_count;
  std::array<engine_tests::ProgramWord, load_index + 2> code{};
  for (unsigned index = 0; index < gpr_count; ++index) {
    code[index] = {
        .offset = index * sizeof(std::uint32_t),
        .instruction = move_wide(index, 0x110U + index),
    };
  }
  for (unsigned index = 0; index < vector_count; ++index) {
    code[gpr_count + index] = {
        .offset = (gpr_count + index) * sizeof(std::uint32_t),
        .instruction =
            simd_modified_immediate(0x40U + index, index),
    };
  }
  code[load_index] = {
      .offset = load_index * sizeof(std::uint32_t),
      .instruction = load_x(21, 20),
  };
  code[load_index + 1] = {
      .offset = (load_index + 1) * sizeof(std::uint32_t),
      .instruction = 0xd400'0001U,
  };
  std::array<std::uint32_t, code.size()> guest{};
  for (std::size_t index = 0; index < code.size(); ++index)
    guest[index] = code[index].instruction;
  const auto compiled = jit_test::compile_block(
      engine_tests::engine_test_code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "forced-miss preservation program did not remain one JIT block");

  constexpr std::size_t data_offset = 128;
  constexpr std::uint64_t loaded_value = 0x5a6b'7c8d'9eaf'b0c1ULL;
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> data{};
  std::memcpy(data.data() + data_offset, &loaded_value,
              sizeof(loaded_value));
  const std::array memory{
      engine_tests::MemoryRegion{
          .address = engine_tests::engine_test_data_base,
          .initial = data,
      },
  };

  arm64::CpuState initial{};
  initial.registers[20] =
      engine_tests::engine_test_data_base + data_offset;
  auto expected = initial;
  for (unsigned index = 0; index < gpr_count; ++index)
    expected.registers[index] = 0x110U + index;
  for (unsigned index = 0; index < vector_count; ++index)
    expected.vectors[index].bytes.fill(
        static_cast<std::byte>(0x40U + index));
  expected.registers[21] = loaded_value;
  expected.pc = engine_tests::engine_test_code_base +
                code.size() * sizeof(std::uint32_t);

  /*
   * This is the first JIT block in a fresh harness. Its private TLB has just
   * been prepared and contains no data entry, so the mapped LDR must take the
   * preserving miss bridge. The first sixteen definitions occupy q16-q31;
   * the final two ordinary definitions occupy otherwise-unbound q6/q7. The
   * terminal flush exposes any register the preserving bridge or nested
   * normal-ABI helper failed to preserve.
   */
  engine_tests::EngineHarness harness;
  harness.check_program({
      .name = "jit-tlb-miss-preserves-all-resident-registers",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::advanced_simd),
      .memory = memory,
      .expected_instructions = code.size(),
  });
  harness.print_summary("arm64-jit-tlb-miss-preservation");
}

void ordinary_low_vectors_commit_on_a_direct_cold_fault() {
  constexpr std::size_t vector_count = 18;
  std::array<engine_tests::ProgramWord, vector_count + 2> code{};
  for (unsigned index = 0; index < vector_count; ++index) {
    code[index] = {
        .offset = index * sizeof(std::uint32_t),
        .instruction =
            simd_modified_immediate(0x70U + index, index),
    };
  }
  code[vector_count] = {
      .offset = vector_count * sizeof(std::uint32_t),
      .instruction = load_x(2U, 20U),
  };
  code[vector_count + 1U] = {
      .offset = (vector_count + 1U) * sizeof(std::uint32_t),
      .instruction = 0xd400'0001U,
  };

  arm64::CpuState initial{};
  initial.registers[20] = 0U;
  auto expected = initial;
  for (unsigned index = 0; index < vector_count; ++index)
    expected.vectors[index].bytes.fill(
        static_cast<std::byte>(0x70U + index));
  expected.pc = engine_tests::engine_test_code_base +
                vector_count * sizeof(std::uint32_t);

  engine_tests::EngineHarness harness;
  harness.check_program({
      .name = "jit-ordinary-low-vectors-commit-on-direct-cold-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::advanced_simd),
      .fault =
          {
              .address = 0U,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = vector_count,
  });
  harness.print_summary("arm64-jit-ordinary-low-vector-cold-fault");
}

void pinned_vectors_commit_on_a_direct_cold_fault() {
  constexpr std::array code{
      engine_tests::ProgramWord{
          .offset = 0U,
          .instruction = indexed_halfword_multiply(0U, 0U, 0U),
      },
      engine_tests::ProgramWord{
          .offset = 4U,
          .instruction = indexed_halfword_multiply(1U, 1U, 1U),
      },
      engine_tests::ProgramWord{
          .offset = 8U,
          .instruction = load_x(2U, 20U),
      },
      engine_tests::ProgramWord{
          .offset = 12U,
          .instruction = 0xd400'0001U,
      },
  };

  arm64::CpuState initial{};
  initial.vectors[0].bytes.fill(static_cast<std::byte>(0x12U));
  initial.vectors[1].bytes.fill(static_cast<std::byte>(0x34U));
  initial.registers[20] = 0U;
  auto expected = initial;
  apply_indexed_halfword_multiply(expected, 0U);
  apply_indexed_halfword_multiply(expected, 1U);
  expected.pc = engine_tests::engine_test_code_base + 8U;

  engine_tests::EngineHarness harness;
  harness.check_program({
      .name = "jit-pinned-vectors-commit-on-direct-cold-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::advanced_simd),
      .fault =
          {
              .address = 0U,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 2U,
  });
  harness.print_summary("arm64-jit-pinned-vector-cold-fault");
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  miss_call_is_cold_and_has_no_resident_spills();
  every_resident_register_survives_a_forced_miss();
  ordinary_low_vectors_commit_on_a_direct_cold_fault();
  pinned_vectors_commit_on_a_direct_cold_fault();
  return 0;
}
