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
#include <limits>
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
    .reg = 2,
    .remap_transfers = {3, 4},
    .tracked_transfers = {3, 4},
    .remap_generation = 9401,
    .remap_base = 0xd0'0000,
    .tracked_generation = 9500,
    .tracked_base = 0xe0'0000,
};

[[nodiscard]] constexpr std::uint32_t
unsigned_x(bool load, unsigned immediate, unsigned base,
           unsigned transfer) noexcept {
  return immediate::encode_unsigned(3, load ? 1U : 0U, immediate, base,
                                    transfer);
}

void every_scalar_variant(engine::EngineHarness &harness) {
  for (std::size_t index = 0; index < immediate::variants.size(); ++index) {
    const auto &variant = immediate::variants[index];
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(index * 19U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc100'0000'0000'0000ULL | (index << 8U));
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[3] = 0x0123'4567'89ab'cdefULL;
    initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(variant.size_code, variant.operation, 3,
                                   2, 3),
        immediate::encode_unsigned(variant.size_code, variant.operation, 1,
                                   2, 4),
    };
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 3, 2,
                            3, expected, expected_memory, data_base);
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 1, 2,
                            4, expected, expected_memory, data_base);
    char name[96];
    std::snprintf(name, sizeof(name), "jit-gpr-run-%.*s",
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
    auto initial = immediate::seeded_state(0xc200'0000'0000'0001ULL);
    initial.registers[2] = data_base + page_size / 4U;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(0, 1, 0, 2, 3),
        immediate::encode_unsigned(0, 2, 1, 2, 3),
        immediate::encode_unsigned(1, 1, 1, 2, 31),
        immediate::encode_unsigned(2, 2, 1, 2, 4),
    };
    const std::array variant_indices{1U, 2U, 5U, 10U};
    const std::array immediates{0U, 1U, 1U, 1U};
    const std::array transfers{3U, 3U, 31U, 4U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 2,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-load-aliases", instructions, initial, expected,
        data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 109);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc200'0000'0000'0002ULL);
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[3] = 0x1122'3344'5566'7788ULL;
    initial.registers[4] = 0xa5;
    initial.registers[5] = 0xbeef;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(3, 0, 0, 2, 2),
        immediate::encode_unsigned(0, 0, 3, 2, 4),
        immediate::encode_unsigned(1, 0, 1, 2, 5),
        immediate::encode_unsigned(2, 0, 1, 2, 31),
    };
    const std::array variant_indices{11U, 0U, 4U, 8U};
    const std::array immediates{0U, 3U, 1U, 1U};
    const std::array transfers{2U, 4U, 5U, 31U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 2,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-store-base-alias", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 113);
    constexpr std::size_t first_offset = 64;
    constexpr std::size_t second_offset = 160;
    const auto second_base = data_base + second_offset;
    immediate::encode_little_endian(memory, first_offset, 8, second_base);
    constexpr std::uint64_t second_value = 0x8877'6655'4433'2211ULL;
    immediate::encode_little_endian(memory, second_offset + 8U, 8,
                                    second_value);
    const auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] = data_base + first_offset;
    initial.registers[3] = 0x1111;
    auto expected = initial;
    expected.registers[2] = second_base;
    expected.registers[3] = second_value;
    const std::array instructions{
        unsigned_x(true, 0, 2, 2),
        unsigned_x(true, 1, 2, 3),
    };
    immediate::check_program(
        harness, "jit-gpr-run-load-base-alias-changes-next-address",
        instructions, initial, expected, data_base, memory, expected_memory);
  }
}

void infrastructure_guest_register_cases(engine::EngineHarness &harness) {
  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 117);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc250'0000'0000'0001ULL);
    initial.registers[8] = data_base + page_size / 4U;
    auto expected = initial;
    const std::array instructions{
        unsigned_x(true, 0, 8, 9),
        unsigned_x(true, 1, 8, 16),
        unsigned_x(true, 2, 8, 17),
        unsigned_x(true, 3, 8, 18),
    };
    constexpr std::array transfers{9U, 16U, 17U, 18U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          static_cast<unsigned>(index), 8, transfers[index], expected,
          expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-infrastructure-register-loads", instructions,
        initial, expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 133);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc250'0000'0000'0002ULL);
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[8] = 0x0123'4567'89ab'cdefULL;
    initial.registers[9] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        unsigned_x(false, 0, 2, 8),
        unsigned_x(false, 1, 2, 9),
    };
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 0, 2, 8,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 1, 2, 9,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-infrastructure-register-stores", instructions,
        initial, expected, data_base, memory, expected_memory);
  }
}

void all_guest_register_cases(engine::EngineHarness &harness) {
  for (unsigned base = 0; base < 31U; ++base) {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 151U + base);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc280'0000'0000'0000ULL | base);
    initial.registers[base] = data_base + page_size / 4U;
    auto expected = initial;
    const unsigned first = (base + 1U) % 31U;
    const unsigned second = (base + 2U) % 31U;
    const std::array instructions{
        unsigned_x(true, 0, base, first),
        unsigned_x(true, 1, base, second),
    };
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 0,
        base, first, expected, expected_memory, data_base);
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 1,
        base, second, expected, expected_memory, data_base);
    char name[80];
    std::snprintf(name, sizeof(name),
                  "jit-gpr-run-all-base-registers-r%u", base);
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }

  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const bool load : {false, true}) {
      std::vector<std::byte> memory(page_size);
      immediate::fill_memory(
          memory, 193U + transfer * 3U + static_cast<unsigned>(load));
      auto expected_memory = memory;
      auto initial = immediate::seeded_state(
          0xc290'0000'0000'0000ULL |
          (static_cast<std::uint64_t>(transfer) << 1U) |
          static_cast<unsigned>(load));
      const unsigned base = transfer == 2U ? 3U : 2U;
      initial.registers[base] = data_base + page_size / 4U;
      auto expected = initial;
      const std::array instructions{
          unsigned_x(load, 0, base, transfer),
          unsigned_x(load, 1, base, transfer),
      };
      const auto &variant =
          immediate::variants[load ? 12U : 11U];
      immediate::apply_oracle(
          variant, immediate::Form::unsigned_offset, 0, base, transfer,
          expected, expected_memory, data_base);
      immediate::apply_oracle(
          variant, immediate::Form::unsigned_offset, 1, base, transfer,
          expected, expected_memory, data_base);
      char name[96];
      std::snprintf(
          name, sizeof(name), "jit-gpr-run-all-transfer-registers-%s-r%u",
          load ? "load" : "store", transfer);
      immediate::check_program(
          harness, name, instructions, initial, expected, data_base, memory,
          expected_memory);
    }
  }
}

void lengths_two_through_six(engine::EngineHarness &harness) {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(length * 31U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc300'0000'0000'0000ULL | length);
    initial.registers[2] = data_base + page_size / 4U;
    auto expected = initial;
    std::vector<std::uint32_t> instructions;
    instructions.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      const auto immediate_value =
          static_cast<unsigned>((length - index) * 2U);
      const auto transfer = static_cast<unsigned>(8U + index);
      instructions.push_back(
          unsigned_x(true, immediate_value, 2, transfer));
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          immediate_value, 2, transfer, expected, expected_memory, data_base);
    }
    char name[64];
    std::snprintf(name, sizeof(name), "jit-gpr-run-length-%zu", length);
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }
}

void exact_page_end_and_runtime_crossing(engine::EngineHarness &harness) {
  constexpr unsigned last_x = (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
  const std::array instructions{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, last_x, 2, 4),
  };

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 141);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc400'0000'0000'0001ULL);
    initial.registers[2] = data_base;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 2, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 2, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-exact-page-end", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(2U * page_size);
    immediate::fill_memory(memory, 157);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc400'0000'0000'0002ULL);
    initial.registers[2] = data_base + 16U;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 2, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 2, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-runtime-page-crossing", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  /* A base in the last bytes of a page whose fields all lie on the next
   * page: the run translates its first accessed byte. */
  for (const std::size_t index : {std::size_t{11}, std::size_t{12}}) {
    const auto &variant = immediate::variants[index];
    const std::array far{
        unsigned_x(variant.operation != 0U, 2, 2, 3),
        unsigned_x(variant.operation != 0U, 3, 2, 4),
    };
    std::vector<std::byte> memory(2U * page_size);
    immediate::fill_memory(memory, 167U + static_cast<unsigned>(index));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc400'0000'0000'0003ULL + index);
    initial.registers[2] = data_base + page_size - 16U;
    auto expected = initial;
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 2, 2,
                            3, expected, expected_memory, data_base);
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 3, 2,
                            4, expected, expected_memory, data_base);
    immediate::check_program(
        harness,
        index == 11U ? "jit-gpr-run-far-store-next-page"
                     : "jit-gpr-run-far-load-next-page",
        far, initial, expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 173);
    auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] =
        std::numeric_limits<std::uint64_t>::max() - 7U;
    auto expected = initial;
    const std::array wrapping{
        unsigned_x(true, 1, 2, 3),
        unsigned_x(true, 2, 2, 4),
    };
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 1, 2, 3,
        expected, expected_memory, 0);
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 2, 2, 4,
        expected, expected_memory, 0);
    immediate::check_program(
        harness, "jit-gpr-run-uint64-address-wrap", wrapping, initial,
        expected, 0, memory, expected_memory);
  }
}

void precise_fault_cases(engine::EngineHarness &harness) {
  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(true, 3, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(true, 4, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    arm64::CpuState initial{};
    initial.registers[2] = data_base + 8U * page_size;
    initial.registers[3] = 0x1111'2222'3333'4444ULL;
    initial.registers[4] = 0x5555'6666'7777'8888ULL;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    harness.check_program({
        .name = "jit-gpr-run-first-translation-fault-address",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = initial.registers[2] + 24U,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .expected_instructions = 0,
    });
  }

  {
    constexpr unsigned last_x =
        (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(false, 0, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(false, last_x, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 173);
    auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] = data_base + 16U;
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
        .name = "jit-gpr-run-later-fault-preserves-first-store",
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

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 189);
    const std::array read_only{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = memory,
            .readable = true,
            .writable = false,
        },
    };
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(false, 2, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(false, 3, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    arm64::CpuState initial{};
    initial.registers[2] = data_base;
    initial.registers[3] = 0x1122'3344'5566'7788ULL;
    initial.registers[4] = 0x99aa'bbcc'ddee'ff00ULL;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    harness.check_program({
        .name = "jit-gpr-run-first-store-permission-fault",
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
        .expected_instructions = 0,
    });
  }
}

void far_run_fault_is_precise(engine::EngineHarness &harness) {
  /* The run translates its first accessed byte, on the unmapped page after
   * the base's: the first operation reports that address and retires
   * nothing. */
  const std::array code{
      engine::ProgramWord{.offset = 0,
                          .instruction = unsigned_x(true, 2, 2, 3)},
      engine::ProgramWord{.offset = 4,
                          .instruction = unsigned_x(true, 3, 2, 4)},
      engine::ProgramWord{.offset = 8, .instruction = system_call},
  };
  std::vector<std::byte> memory(page_size);
  immediate::fill_memory(memory, 181);
  const std::array regions{
      engine::MemoryRegion{
          .address = data_base,
          .initial = memory,
          .expected = memory,
      },
  };
  arm64::CpuState initial{};
  initial.registers[2] = data_base + page_size - 16U;
  initial.registers[3] = 0x1111'2222'3333'4444ULL;
  initial.registers[4] = 0x5555'6666'7777'8888ULL;
  auto expected = initial;
  expected.pc = engine::engine_test_code_base;
  harness.check_program({
      .name = "jit-gpr-run-far-translation-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = engine::NativeRequirements::baseline(),
      .fault =
          {
              .address = data_base + page_size,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .memory = regions,
      .expected_instructions = 0,
  });
}

void straddling_runs_run_natively() {
  /* Base x2; each case's run straddles the first data page's end. */
  namespace straddle = arm64_jit_memory_run_straddle_test;
  using engine::require;
  using straddle::load_x;
  using straddle::pattern_x;
  using straddle::store_x;
  constexpr auto end = page_size;
  const auto guest = [](unsigned index) {
    return std::uint64_t{0xf0'0000} + index * 4U * page_size;
  };
  const auto with_base = [](const straddle::Case &test_case,
                            std::uint64_t offset) {
    arm64::CpuState cpu{};
    cpu.registers[2] = straddle::data(test_case) + offset;
    return cpu;
  };

  {
    const std::array code{load_x(3, 2, 0), load_x(4, 2, 1)};
    const straddle::Case test_case{.name = "jit-gpr-run-straddle-load",
                                   .generation = 9701,
                                   .guest = guest(0),
                                   .run = code};
    straddle::check(test_case, with_base(test_case, end - 8U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end - 8U) &&
                              cpu.registers[4] == pattern_x(end));
                    });
  }
  {
    const std::array code{load_x(3, 2, 2), load_x(4, 2, 0),
                          load_x(5, 2, 1)};
    const straddle::Case test_case{.name = "jit-gpr-run-straddle-any-order",
                                   .generation = 9702,
                                   .guest = guest(1),
                                   .run = code};
    straddle::check(test_case, with_base(test_case, end - 8U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end + 8U) &&
                              cpu.registers[4] == pattern_x(end - 8U) &&
                              cpu.registers[5] == pattern_x(end));
                    });
  }
  {
    const std::array code{load_x(3, 2, 7), load_x(4, 2, 8)};
    const straddle::Case test_case{.name = "jit-gpr-run-straddle-far",
                                   .generation = 9703,
                                   .guest = guest(2),
                                   .run = code};
    straddle::check(test_case, with_base(test_case, end - 64U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end - 8U) &&
                              cpu.registers[4] == pattern_x(end));
                    });
  }
  {
    constexpr std::uint64_t first = 0x0123'4567'89ab'cdefULL;
    constexpr std::uint64_t second = 0xfedc'ba98'7654'3210ULL;
    const std::array code{store_x(3, 2, 0), store_x(4, 2, 1)};
    const straddle::Case test_case{.name = "jit-gpr-run-straddle-store",
                                   .generation = 9704,
                                   .guest = guest(3),
                                   .run = code,
                                   .store = true};
    auto cpu = with_base(test_case, end - 8U);
    cpu.registers[3] = first;
    cpu.registers[4] = second;
    straddle::check(test_case, cpu, [&](const arm64::CpuState &, auto read) {
      require(read(end - 8U) == first && read(end) == second &&
              read(end - 16U) == pattern_x(end - 16U) &&
              read(end + 8U) == pattern_x(end + 8U));
    });
  }
  {
    /* The second access crosses the boundary itself. */
    const std::array code{load_x(3, 2, 0), load_x(4, 2, 1)};
    const straddle::Case test_case{
        .name = "jit-gpr-run-straddle-crossing-access",
        .generation = 9705,
        .guest = guest(4),
        .run = code,
        .native = false};
    straddle::check(test_case, with_base(test_case, end - 12U),
                    [&](const arm64::CpuState &cpu, auto) {
                      require(cpu.registers[3] == pattern_x(end - 12U) &&
                              cpu.registers[4] == pattern_x(end - 4U));
                    });
  }
}

void prefixed_cold_faults_preserve_state(engine::EngineHarness &harness) {
  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = move_wide(6, 0x123U)},
        engine::ProgramWord{.offset = 4,
                            .instruction = compare_x(1, 3)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(true, 0, 10, 4)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(true, 1, 10, 5)},
        engine::ProgramWord{.offset = 16, .instruction = system_call},
    };
    auto initial = immediate::seeded_state(0xc500'0000'0000'0001ULL);
    initial.registers[1] = 7;
    initial.registers[3] = 7;
    initial.registers[10] = data_base + 8U * page_size;
    const auto untouched_first = initial.registers[4];
    const auto untouched_second = initial.registers[5];
    auto expected = initial;
    expected.registers[6] = 0x123U;
    expected.registers[4] = untouched_first;
    expected.registers[5] = untouched_second;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-gpr-run-prefixed-translation-cold-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = initial.registers[10],
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .expected_instructions = 2,
    });
  }

  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = move_wide(6, 0x456U)},
        engine::ProgramWord{.offset = 4,
                            .instruction = compare_x(1, 3)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(false, 2, 10, 4)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(false, 3, 10, 5)},
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
    auto initial = immediate::seeded_state(0xc500'0000'0000'0002ULL);
    initial.registers[1] = 11;
    initial.registers[3] = 11;
    initial.registers[10] = data_base;
    auto expected = initial;
    expected.registers[6] = 0x456U;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-gpr-run-prefixed-permission-cold-fault",
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

} // namespace

int main() {
  engine::require(arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 3));
  engine::EngineHarness harness;
  every_scalar_variant(harness);
  alias_and_order_cases(harness);
  infrastructure_guest_register_cases(harness);
  all_guest_register_cases(harness);
  lengths_two_through_six(harness);
  exact_page_end_and_runtime_crossing(harness);
  precise_fault_cases(harness);
  far_run_fault_is_precise(harness);
  prefixed_cold_faults_preserve_state(harness);
  straddling_runs_run_natively();
  run::finish(harness, "arm64-jit-gpr-memory-run-runtime", scalar_base);
  return 0;
}
