// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"
#include "arm64_jit_memory_run_runtime_test_support.hpp"
#include "arm64_jit_memory_run_straddle_test_support.hpp"
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;
namespace immediate = uml::emulator::tests::base_immediate_memory;
namespace run = arm64_jit_memory_run_runtime_test;

namespace {

using run::compare_x;
using run::data_base;
using run::move_wide;
using run::page_size;
using run::system_call;

constexpr run::ScalarBase scalar_base{
    .reg = 31,
    .remap_transfers = {1, 2},
    .tracked_transfers = {2, 3},
    .remap_generation = 9201,
    .remap_base = 0xb0'0000,
    .tracked_generation = 9300,
    .tracked_base = 0xc0'0000,
};

[[nodiscard]] constexpr std::uint32_t
unsigned_x(bool load, unsigned immediate, unsigned transfer) noexcept {
  return immediate::encode_unsigned(3, load ? 1U : 0U, immediate, 31,
                                    transfer);
}

void every_scalar_variant(engine::EngineHarness &harness) {
  for (std::size_t index = 0; index < immediate::variants.size(); ++index) {
    const auto &variant = immediate::variants[index];
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(index * 17U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xb100'0000'0000'0000ULL | (index << 8U));
    initial.sp = data_base + page_size / 4U;
    initial.registers[3] = 0x0123'4567'89ab'cdefULL;
    initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(variant.size_code, variant.operation, 3,
                                   31, 3),
        immediate::encode_unsigned(variant.size_code, variant.operation, 1,
                                   31, 4),
    };
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 3, 31,
                            3, expected, expected_memory, data_base);
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 1, 31,
                            4, expected, expected_memory, data_base);
    char name[96];
    std::snprintf(name, sizeof(name), "jit-sp-run-%.*s",
                  static_cast<int>(variant.name.size()),
                  variant.name.data());
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }
}

void alias_and_order_cases(engine::EngineHarness &harness) {
  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 91);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb200'0000'0000'0001ULL);
    initial.sp = data_base + page_size / 4U;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(0, 1, 0, 31, 3),
        immediate::encode_unsigned(0, 2, 1, 31, 3),
        immediate::encode_unsigned(1, 1, 1, 31, 31),
        immediate::encode_unsigned(2, 2, 1, 31, 4),
    };
    const std::array variant_indices{1U, 2U, 5U, 10U};
    const std::array immediates{0U, 1U, 1U, 1U};
    const std::array transfers{3U, 3U, 31U, 4U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 31,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-load-aliases", instructions, initial, expected,
        data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 109);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb200'0000'0000'0002ULL);
    initial.sp = data_base + page_size / 4U;
    initial.registers[3] = 0x1122'3344'5566'7788ULL;
    initial.registers[4] = 0xa5;
    initial.registers[5] = 0xbeef;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(3, 0, 0, 31, 3),
        immediate::encode_unsigned(0, 0, 3, 31, 4),
        immediate::encode_unsigned(1, 0, 1, 31, 5),
        immediate::encode_unsigned(2, 0, 1, 31, 31),
    };
    const std::array variant_indices{11U, 0U, 4U, 8U};
    const std::array immediates{0U, 3U, 1U, 1U};
    const std::array transfers{3U, 4U, 5U, 31U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 31,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-overlapping-stores", instructions, initial,
        expected, data_base, memory, expected_memory);
  }
}

void infrastructure_guest_register_cases(engine::EngineHarness &harness) {
  {
    constexpr std::array transfers{8U, 9U, 16U, 17U, 18U, 8U};
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 117);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb250'0000'0000'0001ULL);
    initial.sp = data_base + page_size / 4U;
    auto expected = initial;
    std::array<std::uint32_t, transfers.size()> instructions{};
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      instructions[index] =
          unsigned_x(true, static_cast<unsigned>(index), transfers[index]);
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          static_cast<unsigned>(index), 31, transfers[index], expected,
          expected_memory, data_base);
    }
    immediate::check_program(
        harness, "jit-sp-run-infrastructure-register-loads", instructions,
        initial, expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 133);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb250'0000'0000'0002ULL);
    initial.sp = data_base + page_size / 4U;
    initial.registers[8] = 0x0123'4567'89ab'cdefULL;
    initial.registers[9] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        unsigned_x(false, 0, 8),
        unsigned_x(false, 1, 9),
    };
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 0, 31, 8,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 1, 31, 9,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-infrastructure-register-stores", instructions,
        initial, expected, data_base, memory, expected_memory);
  }
}

void lengths_two_through_six(engine::EngineHarness &harness) {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(length * 31U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xb300'0000'0000'0000ULL | length);
    initial.sp = data_base + page_size / 4U;
    auto expected = initial;
    std::vector<std::uint32_t> instructions;
    instructions.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      const auto immediate_value =
          static_cast<unsigned>((length - index) * 2U);
      const auto transfer = static_cast<unsigned>(8U + index);
      instructions.push_back(
          unsigned_x(true, immediate_value, transfer));
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          immediate_value, 31, transfer, expected, expected_memory, data_base);
    }
    char name[64];
    std::snprintf(name, sizeof(name), "jit-sp-run-length-%zu", length);
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }
}

void exact_page_end_and_runtime_crossing(engine::EngineHarness &harness) {
  constexpr unsigned last_x = (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
  const std::array instructions{
      unsigned_x(true, 0, 3),
      unsigned_x(true, last_x, 4),
  };

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 141);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb400'0000'0000'0001ULL);
    initial.sp = data_base;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 31, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 31, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-exact-page-end", instructions, initial, expected,
        data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(2U * page_size);
    immediate::fill_memory(memory, 157);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb400'0000'0000'0002ULL);
    initial.sp = data_base + 16U;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 31, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 31, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-runtime-page-crossing", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  /* SP in the last bytes of a page with its slots on the next page. */
  {
    const std::array far{
        unsigned_x(true, 2, 3),
        unsigned_x(true, 3, 4),
    };
    std::vector<std::byte> memory(2U * page_size);
    immediate::fill_memory(memory, 163);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xb400'0000'0000'0003ULL);
    initial.sp = data_base + page_size - 16U;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 2, 31, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 3, 31, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-sp-run-far-load-next-page", far, initial, expected,
        data_base, memory, expected_memory);
  }
}

void first_translation_fault_is_effective_address(
    engine::EngineHarness &harness) {
  const std::array code{
      engine::ProgramWord{.offset = 0,
                          .instruction = unsigned_x(true, 3, 3)},
      engine::ProgramWord{.offset = 4,
                          .instruction = unsigned_x(true, 4, 4)},
      engine::ProgramWord{.offset = 8, .instruction = system_call},
  };
  arm64::CpuState initial{};
  initial.sp = data_base + 8U * page_size;
  initial.registers[3] = 0x1111'2222'3333'4444ULL;
  initial.registers[4] = 0x5555'6666'7777'8888ULL;
  auto expected = initial;
  expected.pc = engine::engine_test_code_base;
  harness.check_program({
      .name = "jit-sp-run-first-translation-fault-address",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = engine::NativeRequirements::baseline(),
      .fault =
          {
              .address = initial.sp + 24U,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 0,
  });
}

void later_fault_preserves_first_store(engine::EngineHarness &harness) {
  constexpr unsigned last_x = (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
  const std::array code{
      engine::ProgramWord{.offset = 0,
                          .instruction = unsigned_x(false, 0, 3)},
      engine::ProgramWord{.offset = 4,
                          .instruction = unsigned_x(false, last_x, 4)},
      engine::ProgramWord{.offset = 8, .instruction = system_call},
  };
  std::vector<std::byte> memory(page_size);
  immediate::fill_memory(memory, 173);
  auto expected_memory = memory;
  arm64::CpuState initial{};
  initial.sp = data_base + 16U;
  initial.registers[3] = 0x0123'4567'89ab'cdefULL;
  initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
  auto expected = initial;
  immediate::encode_little_endian(expected_memory, 16, 8,
                                  initial.registers[3]);
  expected.pc = engine::engine_test_code_base + 4U;
  const std::array regions{
      engine::MemoryRegion{
          .address = data_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = "jit-sp-run-later-fault-preserves-first-store",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = engine::NativeRequirements::baseline(),
      .fault =
          {
              .address = data_base + page_size + 8U,
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
      .memory = regions,
      .expected_instructions = 1,
  });
}

void alignment_and_permission_faults(engine::EngineHarness &harness) {
  const std::array code{
      engine::ProgramWord{.offset = 0,
                          .instruction = unsigned_x(true, 0, 3)},
      engine::ProgramWord{.offset = 4,
                          .instruction = unsigned_x(true, 1, 4)},
      engine::ProgramWord{.offset = 8, .instruction = system_call},
  };
  std::vector<std::byte> memory(page_size);
  immediate::fill_memory(memory, 189);
  const std::array readable{
      engine::MemoryRegion{
          .address = data_base,
          .initial = memory,
          .expected = memory,
      },
  };
  arm64::CpuState initial{};
  initial.sp = data_base + 1U;
  initial.registers[3] = 0x1234;
  initial.registers[4] = 0x5678;
  auto expected = initial;
  expected.pc = engine::engine_test_code_base;
  harness.check_program({
      .name = "jit-sp-run-alignment-precedes-memory",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = engine::NativeRequirements::baseline(),
      .fault =
          {
              .address = initial.sp,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .memory = readable,
      .expected_instructions = 0,
  });

  const std::array store_code{
      engine::ProgramWord{.offset = 0,
                          .instruction = unsigned_x(false, 2, 3)},
      engine::ProgramWord{.offset = 4,
                          .instruction = unsigned_x(false, 3, 4)},
      engine::ProgramWord{.offset = 8, .instruction = system_call},
  };
  const std::array read_only{
      engine::MemoryRegion{
          .address = data_base,
          .initial = memory,
          .expected = memory,
          .readable = true,
          .writable = false,
      },
  };
  initial = {};
  initial.sp = data_base;
  initial.registers[3] = 0x1122'3344'5566'7788ULL;
  initial.registers[4] = 0x99aa'bbcc'ddee'ff00ULL;
  expected = initial;
  expected.pc = engine::engine_test_code_base;
  harness.check_program({
      .name = "jit-sp-run-first-store-permission-fault",
      .code = store_code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = engine::NativeRequirements::baseline(),
      .fault =
          {
              .address = data_base + 16U,
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
      .memory = read_only,
      .expected_instructions = 0,
  });
}

void prefixed_cold_faults_preserve_state(engine::EngineHarness &harness) {
  {
    const std::array code{
        engine::ProgramWord{.offset = 0, .instruction = move_wide(3, 0x123U)},
        engine::ProgramWord{.offset = 4, .instruction = compare_x(1, 2)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(true, 0, 3)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(true, 1, 4)},
        engine::ProgramWord{.offset = 16, .instruction = system_call},
    };
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 197);
    const std::array readable{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = memory,
        },
    };
    auto initial = immediate::seeded_state(0xb500'0000'0000'0001ULL);
    initial.sp = data_base + 1U;
    initial.registers[1] = 7;
    initial.registers[2] = 7;
    const auto untouched_second = initial.registers[4];
    auto expected = initial;
    expected.registers[3] = 0x123U;
    expected.registers[4] = untouched_second;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-sp-run-prefixed-alignment-cold-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = initial.sp,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .memory = readable,
        .expected_instructions = 2,
    });
  }

  {
    const std::array code{
        engine::ProgramWord{.offset = 0, .instruction = move_wide(8, 0x456U)},
        engine::ProgramWord{.offset = 4, .instruction = compare_x(1, 2)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(false, 2, 8)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(false, 3, 9)},
        engine::ProgramWord{.offset = 16, .instruction = system_call},
    };
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 213);
    const std::array read_only{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = memory,
            .readable = true,
            .writable = false,
        },
    };
    auto initial = immediate::seeded_state(0xb500'0000'0000'0002ULL);
    initial.sp = data_base;
    initial.registers[1] = 11;
    initial.registers[2] = 11;
    auto expected = initial;
    expected.registers[8] = 0x456U;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-sp-run-prefixed-permission-cold-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = data_base + 16U,
                .access = arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = read_only,
        .expected_instructions = 2,
    });
  }
}

void miss_preserves_resident_prefix(engine::EngineHarness &harness) {
  constexpr auto region_base = data_base + 32U * page_size;
  std::vector<std::byte> memory(page_size);
  immediate::fill_memory(memory, 211);
  constexpr std::uint64_t first = 0x1020'3040'5060'7080ULL;
  constexpr std::uint64_t second = 0x8877'6655'4433'2211ULL;
  immediate::encode_little_endian(memory, 32, 8, first);
  immediate::encode_little_endian(memory, 40, 8, second);
  const auto expected_memory = memory;

  std::array<engine::ProgramWord, 11> code{};
  for (unsigned index = 0; index < 8; ++index)
    code[index] = {
        .offset = index * sizeof(std::uint32_t),
        .instruction = move_wide(index, 0x120U + index),
    };
  code[8] = {.offset = 32, .instruction = unsigned_x(true, 4, 8)};
  code[9] = {.offset = 36, .instruction = unsigned_x(true, 5, 9)};
  code[10] = {.offset = 40, .instruction = system_call};
  arm64::CpuState initial{};
  initial.sp = region_base;
  auto expected = initial;
  for (unsigned index = 0; index < 8; ++index)
    expected.registers[index] = 0x120U + index;
  expected.registers[8] = first;
  expected.registers[9] = second;
  expected.pc = engine::engine_test_code_base + 44U;
  const std::array regions{
      engine::MemoryRegion{
          .address = region_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = "jit-sp-run-miss-preserves-resident-prefix",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine::NativeRequirements::baseline(),
      .memory = regions,
      .expected_instructions = code.size(),
  });
}

void straddling_runs_run_natively() {
  /* Each case's run straddles the first data page's end. */
  namespace straddle = arm64_jit_memory_run_straddle_test;
  using engine::require;
  using straddle::load_x;
  using straddle::pattern_x;
  using straddle::store_x;
  constexpr auto end = page_size;
  const auto guest = [](unsigned index) {
    return std::uint64_t{0xf0'0000} + index * 4U * page_size;
  };
  const auto with_sp = [](const straddle::Case &test_case,
                          std::uint64_t offset) {
    arm64::CpuState cpu{};
    cpu.sp = straddle::data(test_case) + offset;
    return cpu;
  };

  {
    const std::array code{load_x(3, 31, 0), load_x(4, 31, 2),
                          load_x(5, 31, 1)};
    const straddle::Case test_case{.name = "jit-sp-run-straddle-load",
                                   .generation = 9711,
                                   .guest = guest(0),
                                   .run = code};
    straddle::check(test_case, with_sp(test_case, end - 16U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end - 16U) &&
                              cpu.registers[4] == pattern_x(end) &&
                              cpu.registers[5] == pattern_x(end - 8U));
                    });
  }
  {
    const std::array code{load_x(3, 31, 7), load_x(4, 31, 8)};
    const straddle::Case test_case{.name = "jit-sp-run-straddle-far",
                                   .generation = 9712,
                                   .guest = guest(1),
                                   .run = code};
    straddle::check(test_case, with_sp(test_case, end - 64U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end - 8U) &&
                              cpu.registers[4] == pattern_x(end));
                    });
  }
  {
    constexpr std::uint64_t first = 0x1357'9bdf'0246'8aceULL;
    constexpr std::uint64_t second = 0xeca8'6420'fdb9'7531ULL;
    const std::array code{store_x(3, 31, 1), store_x(4, 31, 2)};
    const straddle::Case test_case{.name = "jit-sp-run-straddle-store",
                                   .generation = 9713,
                                   .guest = guest(2),
                                   .run = code,
                                   .store = true};
    auto cpu = with_sp(test_case, end - 16U);
    cpu.registers[3] = first;
    cpu.registers[4] = second;
    straddle::check(test_case, cpu, [&](const arm64::CpuState &, auto read) {
      require(read(end - 8U) == first && read(end) == second &&
              read(end - 16U) == pattern_x(end - 16U) &&
              read(end + 8U) == pattern_x(end + 8U));
    });
  }
}

} // namespace

int main() {
  engine::require(arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 3));
  engine::EngineHarness harness;
  every_scalar_variant(harness);
  alias_and_order_cases(harness);
  infrastructure_guest_register_cases(harness);
  lengths_two_through_six(harness);
  exact_page_end_and_runtime_crossing(harness);
  first_translation_fault_is_effective_address(harness);
  later_fault_preserves_first_store(harness);
  alignment_and_permission_faults(harness);
  prefixed_cold_faults_preserve_state(harness);
  miss_preserves_resident_prefix(harness);
  straddling_runs_run_natively();
  run::finish(harness, "arm64-jit-sp-memory-run-runtime", scalar_base);
  return 0;
}
