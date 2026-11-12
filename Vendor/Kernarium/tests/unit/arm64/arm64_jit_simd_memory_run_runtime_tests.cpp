// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_memory_run_straddle_test_support.hpp"
#include "arm64_simd_ordinary_memory_test_support.hpp"
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;
namespace memory = arm64_simd_ordinary_memory_test;

namespace {

constexpr auto page_size = std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE};
constexpr auto data_base = engine::engine_test_data_base;
constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
simd_modified_immediate(unsigned immediate, unsigned destination) noexcept {
  return 0x4f00'0400U | ((immediate & 0xe0U) << 11U) | (14U << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_x(unsigned left, unsigned right) noexcept {
  return 0xeb00'001fU | ((right & 31U) << 16U) | ((left & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
load_exclusive_byte(unsigned base, unsigned transfer) noexcept {
  return 0x085f'7c00U | ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
store_exclusive_byte(unsigned status, unsigned base,
                     unsigned transfer) noexcept {
  return 0x0800'7c00U | ((status & 31U) << 16U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint64_t
tagged(std::uint64_t address, std::uint8_t tag) noexcept {
  return (address & 0x00ff'ffff'ffff'ffffULL) |
         (static_cast<std::uint64_t>(tag & 15U) << 56U);
}

void check_success(
    engine::EngineHarness &harness, std::string_view name,
    std::span<const std::uint32_t> instructions,
    const arm64::CpuState &initial, arm64::CpuState expected,
    std::uint64_t region_address, std::span<const std::byte> initial_memory,
    std::span<const std::byte> expected_memory) {
  std::vector<engine::ProgramWord> code;
  code.reserve(instructions.size() + 1U);
  for (std::size_t index = 0; index < instructions.size(); ++index)
    code.push_back({
        .offset = index * sizeof(std::uint32_t),
        .instruction = instructions[index],
    });
  code.push_back({
      .offset = instructions.size() * sizeof(std::uint32_t),
      .instruction = system_call,
  });
  expected.pc =
      engine::engine_test_code_base + code.size() * sizeof(std::uint32_t);
  const std::array regions{
      engine::MemoryRegion{
          .address = region_address,
          .initial = initial_memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = memory::single_native(),
      .memory = regions,
      .expected_instructions = code.size(),
  });
}

void apply_unsigned_oracle(
    const arm64::CpuState &initial, arm64::CpuState &expected,
    std::span<const std::byte> initial_memory,
    std::span<std::byte> expected_memory, std::uint64_t physical_base,
    const memory::SingleVariant &variant, unsigned immediate,
    unsigned transfer) {
  const auto address = physical_base + immediate * variant.size;
  memory::apply_single_oracle(
      initial, expected, initial_memory, expected_memory,
      static_cast<std::size_t>(address - data_base), variant, transfer);
}

void every_valid_variant_for_sp_and_gpr(engine::EngineHarness &harness) {
  for (const unsigned base_register : {31U, 20U}) {
    for (std::size_t index = 0; index < memory::single_variants.size();
         ++index) {
      const auto &variant = memory::single_variants[index];
      std::vector<std::byte> initial_memory(page_size);
      memory::fill_memory(initial_memory, index * 19U + base_register);
      auto expected_memory = initial_memory;
      auto initial =
          memory::seeded_state(0xd100'0000'0000'0000ULL |
                               (base_register << 8U) | index);
      const auto base = data_base + page_size / 4U;
      memory::write_base(initial, base_register, base);
      auto expected = initial;
      const std::array instructions{
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 1, base_register,
              static_cast<unsigned>(index)),
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 3, base_register,
              static_cast<unsigned>(31U - index)),
      };
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          1, static_cast<unsigned>(index));
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          3, static_cast<unsigned>(31U - index));
      char name[128];
      std::snprintf(
          name, sizeof(name), "jit-simd-run-%.*s-%s",
          static_cast<int>(variant.name.size()), variant.name.data(),
          base_register == 31U ? "sp" : "gpr");
      check_success(
          harness, name, instructions, initial, expected, data_base,
          initial_memory, expected_memory);
    }
  }
}

void every_vector_register(engine::EngineHarness &harness) {
  constexpr auto store = memory::single_variants[8];
  constexpr auto load = memory::single_variants[9];
  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const auto &variant : {store, load}) {
      const unsigned base_register = (transfer & 1U) == 0U ? 31U : 20U;
      const auto base = data_base + page_size / 4U;
      std::vector<std::byte> initial_memory(page_size);
      memory::fill_memory(initial_memory, transfer * 23U + variant.load);
      auto expected_memory = initial_memory;
      auto initial = memory::seeded_state(
          0xd200'0000'0000'0000ULL | (transfer << 4U) | variant.load);
      memory::write_base(initial, base_register, base);
      auto expected = initial;
      const std::array instructions{
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 4, base_register,
              transfer),
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 5, base_register,
              transfer),
      };
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          4, transfer);
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          5, transfer);
      char name[96];
      std::snprintf(
          name, sizeof(name), "jit-simd-run-v%u-%s", transfer,
          variant.load ? "load" : "store");
      check_success(
          harness, name, instructions, initial, expected, data_base,
          initial_memory, expected_memory);
    }
  }
}

void lengths_two_through_ten(engine::EngineHarness &harness) {
  const auto &variant = memory::single_variants[9];
  for (const unsigned base_register : {31U, 21U}) {
    for (std::size_t length = 2; length <= 10; ++length) {
      std::vector<std::byte> initial_memory(page_size);
      memory::fill_memory(
          initial_memory, static_cast<unsigned>(length * 29U + base_register));
      auto expected_memory = initial_memory;
      auto initial = memory::seeded_state(
          0xd300'0000'0000'0000ULL | (base_register << 8U) | length);
      const auto base = data_base + page_size / 4U;
      memory::write_base(initial, base_register, base);
      auto expected = initial;
      std::vector<std::uint32_t> instructions;
      instructions.reserve(length);
      for (std::size_t index = 0; index < length; ++index) {
        const auto immediate =
            static_cast<unsigned>((length - index) * 2U);
        const auto transfer = static_cast<unsigned>(index) & 31U;
        instructions.push_back(memory::unsigned_instruction(
            variant.size_code, variant.operation, immediate, base_register,
            transfer));
        apply_unsigned_oracle(
            initial, expected, initial_memory, expected_memory, base, variant,
            immediate, transfer);
      }
      char name[96];
      std::snprintf(
          name, sizeof(name), "jit-simd-run-length-%zu-%s", length,
          base_register == 31U ? "sp" : "gpr");
      check_success(
          harness, name, instructions, initial, expected, data_base,
          initial_memory, expected_memory);
    }
  }
}

void exact_page_edges_and_runtime_crossing(
    engine::EngineHarness &harness) {
  const auto &variant = memory::single_variants[9];
  constexpr unsigned last_q = (DARWIN_UML_GUEST_PAGE_SIZE - 16U) / 16U;
  for (const unsigned base_register : {31U, 22U}) {
    {
      std::vector<std::byte> initial_memory(page_size);
      memory::fill_memory(initial_memory, base_register + 71U);
      auto expected_memory = initial_memory;
      auto initial = memory::seeded_state(
          0xd400'0000'0000'0000ULL | base_register);
      memory::write_base(initial, base_register, data_base);
      auto expected = initial;
      const std::array instructions{
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 0, base_register, 0),
          memory::unsigned_instruction(
              variant.size_code, variant.operation, last_q, base_register,
              31),
      };
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, data_base,
          variant, 0, 0);
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, data_base,
          variant, last_q, 31);
      check_success(
          harness,
          base_register == 31U ? "jit-simd-run-exact-page-end-sp"
                               : "jit-simd-run-exact-page-end-gpr",
          instructions, initial, expected, data_base, initial_memory,
          expected_memory);
    }

    {
      std::vector<std::byte> initial_memory(2U * page_size);
      memory::fill_memory(initial_memory, base_register + 89U);
      auto expected_memory = initial_memory;
      auto initial = memory::seeded_state(
          0xd400'0000'0000'0100ULL | base_register);
      const auto base = data_base + 16U;
      memory::write_base(initial, base_register, base);
      auto expected = initial;
      const std::array instructions{
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 0, base_register, 1),
          memory::unsigned_instruction(
              variant.size_code, variant.operation, last_q, base_register,
              2),
      };
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          0, 1);
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          last_q, 2);
      check_success(
          harness,
          base_register == 31U ? "jit-simd-run-runtime-crossing-sp"
                               : "jit-simd-run-runtime-crossing-gpr",
          instructions, initial, expected, data_base, initial_memory,
          expected_memory);
    }

    /* A base in the last bytes of a page whose fields all lie on the next
     * page. */
    {
      std::vector<std::byte> initial_memory(2U * page_size);
      memory::fill_memory(initial_memory, base_register + 97U);
      auto expected_memory = initial_memory;
      auto initial = memory::seeded_state(
          0xd400'0000'0000'0200ULL | base_register);
      const auto base = data_base + page_size - 16U;
      memory::write_base(initial, base_register, base);
      auto expected = initial;
      const std::array instructions{
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 1, base_register, 3),
          memory::unsigned_instruction(
              variant.size_code, variant.operation, 2, base_register, 4),
      };
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          1, 3);
      apply_unsigned_oracle(
          initial, expected, initial_memory, expected_memory, base, variant,
          2, 4);
      check_success(
          harness,
          base_register == 31U ? "jit-simd-run-far-next-page-sp"
                               : "jit-simd-run-far-next-page-gpr",
          instructions, initial, expected, data_base, initial_memory,
          expected_memory);
    }
  }
}

void first_and_later_faults(engine::EngineHarness &harness) {
  const auto &load = memory::single_variants[9];
  const auto &store = memory::single_variants[8];
  constexpr unsigned last_q = (DARWIN_UML_GUEST_PAGE_SIZE - 16U) / 16U;

  {
    const std::array code{
        engine::ProgramWord{
            .offset = 0,
            .instruction = memory::unsigned_instruction(
                load.size_code, load.operation, 2, 23, 0),
        },
        engine::ProgramWord{
            .offset = 4,
            .instruction = memory::unsigned_instruction(
                load.size_code, load.operation, 3, 23, 1),
        },
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    arm64::CpuState initial{};
    initial.registers[23] = data_base + 8U * page_size;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    harness.check_program({
        .name = "jit-simd-run-first-translation-fault-address",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = memory::single_native(),
        .fault =
            {
                .address = initial.registers[23] + 32U,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .expected_instructions = 0,
    });
  }

  {
    const std::array code{
        engine::ProgramWord{
            .offset = 0,
            .instruction = memory::unsigned_instruction(
                store.size_code, store.operation, 0, 31, 3),
        },
        engine::ProgramWord{
            .offset = 4,
            .instruction = memory::unsigned_instruction(
                store.size_code, store.operation, last_q, 31, 4),
        },
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    std::vector<std::byte> initial_memory(page_size);
    memory::fill_memory(initial_memory, 131);
    auto expected_memory = initial_memory;
    auto initial = memory::seeded_state(0xd500'0000'0000'0001ULL);
    initial.sp = data_base + 16U;
    auto expected = initial;
    std::copy_n(
        initial.vectors[3].bytes.begin(), store.size,
        expected_memory.begin() + 16);
    expected.pc = engine::engine_test_code_base + 4U;
    const std::array regions{
        engine::MemoryRegion{
            .address = data_base,
            .initial = initial_memory,
            .expected = expected_memory,
        },
    };
    harness.check_program({
        .name = "jit-simd-run-later-fault-preserves-first-store",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = memory::single_native(),
        .fault =
            {
                .address = data_base + page_size,
                .access = arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = regions,
        .expected_instructions = 1,
    });
  }
}

void alignment_and_permission_faults(engine::EngineHarness &harness) {
  const auto &load = memory::single_variants[9];
  const auto &store = memory::single_variants[8];
  std::vector<std::byte> initial_memory(page_size);
  memory::fill_memory(initial_memory, 149);

  {
    const std::array code{
        engine::ProgramWord{
            .offset = 0,
            .instruction = memory::unsigned_instruction(
                load.size_code, load.operation, 0, 31, 0),
        },
        engine::ProgramWord{
            .offset = 4,
            .instruction = memory::unsigned_instruction(
                load.size_code, load.operation, 1, 31, 1),
        },
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    auto initial = memory::seeded_state(0xd600'0000'0000'0001ULL);
    initial.sp = data_base + 1U;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    const std::array regions{
        engine::MemoryRegion{
            .address = data_base,
            .initial = initial_memory,
            .expected = initial_memory,
        },
    };
    harness.check_program({
        .name = "jit-simd-run-sp-alignment-precedes-memory",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = memory::single_native(),
        .fault =
            {
                .address = initial.sp,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .memory = regions,
        .expected_instructions = 0,
    });
  }

  for (const bool load_operation : {false, true}) {
    const auto &variant = load_operation ? load : store;
    const std::array code{
        engine::ProgramWord{
            .offset = 0,
            .instruction = memory::unsigned_instruction(
                variant.size_code, variant.operation, 1, 24, 2),
        },
        engine::ProgramWord{
            .offset = 4,
            .instruction = memory::unsigned_instruction(
                variant.size_code, variant.operation, 2, 24, 3),
        },
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    auto initial = memory::seeded_state(
        0xd600'0000'0000'0100ULL | load_operation);
    initial.registers[24] = data_base;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    const std::array regions{
        engine::MemoryRegion{
            .address = data_base,
            .initial = initial_memory,
            .expected = initial_memory,
            .readable = !load_operation,
            .writable = load_operation,
        },
    };
    harness.check_program({
        .name =
            load_operation ? "jit-simd-run-first-load-permission-fault"
                           : "jit-simd-run-first-store-permission-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = memory::single_native(),
        .fault =
            {
                .address = data_base + 16U,
                .access = load_operation ? arm64::MemoryAccess::read
                                         : arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = regions,
        .expected_instructions = 0,
    });
  }
}

void dirty_prefix_is_preserved_on_cold_fallback(
    engine::EngineHarness &harness) {
  const auto &store = memory::single_variants[8];
  const std::array code{
      engine::ProgramWord{.offset = 0,
                          .instruction = move_wide(5, 0x456U)},
      engine::ProgramWord{
          .offset = 4,
          .instruction = simd_modified_immediate(0xa7U, 6)},
      engine::ProgramWord{.offset = 8, .instruction = compare_x(1, 2)},
      engine::ProgramWord{
          .offset = 12,
          .instruction = memory::unsigned_instruction(
              store.size_code, store.operation, 1, 25, 6),
      },
      engine::ProgramWord{
          .offset = 16,
          .instruction = memory::unsigned_instruction(
              store.size_code, store.operation, 2, 25, 7),
      },
      engine::ProgramWord{.offset = 20, .instruction = system_call},
  };
  std::vector<std::byte> initial_memory(page_size);
  memory::fill_memory(initial_memory, 167);
  auto initial = memory::seeded_state(0xd700'0000'0000'0001ULL);
  initial.registers[1] = 9;
  initial.registers[2] = 9;
  initial.registers[25] = data_base;
  auto expected = initial;
  expected.registers[5] = 0x456U;
  expected.vectors[6].bytes.fill(std::byte{0xa7});
  expected.nzcv = 0x6000'0000U;
  expected.pc = engine::engine_test_code_base + 12U;
  const std::array regions{
      engine::MemoryRegion{
          .address = data_base,
          .initial = initial_memory,
          .expected = initial_memory,
          .readable = true,
          .writable = false,
      },
  };
  harness.check_program({
      .name = "jit-simd-run-dirty-prefix-permission-fallback",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = memory::single_native(),
      .fault =
          {
              .address = data_base + 16U,
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
      .memory = regions,
      .expected_instructions = 3,
  });
}

void tagged_state_fallback_is_exact(engine::EngineHarness &harness) {
  constexpr std::uint8_t address_tag = 9;
  const auto &load = memory::single_variants[9];
  const auto physical_base = data_base + page_size / 4U;
  const auto logical_base = tagged(physical_base, address_tag);
  std::vector<std::byte> initial_memory(page_size);
  memory::fill_memory(initial_memory, 181);
  const std::array instructions{
      memory::unsigned_instruction(
          load.size_code, load.operation, 0, 26, 8),
      memory::unsigned_instruction(
          load.size_code, load.operation, 1, 26, 9),
  };

  {
    auto initial = memory::seeded_state(0xd800'0000'0000'0001ULL);
    initial.tbi_enabled = true;
    initial.registers[26] = logical_base;
    auto expected = initial;
    auto expected_memory = initial_memory;
    apply_unsigned_oracle(
        initial, expected, initial_memory, expected_memory, physical_base,
        load, 0, 8);
    apply_unsigned_oracle(
        initial, expected, initial_memory, expected_memory, physical_base,
        load, 1, 9);
    check_success(
        harness, "jit-simd-run-tagged-state-untagged", instructions, initial,
        expected, data_base, initial_memory, expected_memory);
  }

  {
    /* The second load crosses into the unmapped page after the region. */
    const auto edge_base = data_base + page_size - load.size;
    auto initial = memory::seeded_state(0xd800'0000'0000'0002ULL);
    initial.tbi_enabled = true;
    initial.registers[26] = tagged(edge_base, address_tag);
    auto expected = initial;
    auto expected_memory = initial_memory;
    apply_unsigned_oracle(
        initial, expected, initial_memory, expected_memory, edge_base,
        load, 0, 8);
    expected.pc = engine::engine_test_code_base + 4U;
    const std::array code{
        engine::ProgramWord{.offset = 0, .instruction = instructions[0]},
        engine::ProgramWord{.offset = 4, .instruction = instructions[1]},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    const std::array regions{
        engine::MemoryRegion{
            .address = data_base,
            .initial = initial_memory,
            .expected = expected_memory,
        },
    };
    harness.check_program({
        .name = "jit-simd-run-tagged-state-second-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = memory::single_native(),
        .fault =
            {
                .address = data_base + page_size,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .memory = regions,
        .expected_instructions = 1,
    });
  }

  {
    auto initial = memory::seeded_state(0xd800'0000'0000'0003ULL);
    initial.tbi_enabled = true;
    initial.sp = physical_base;
    auto expected = initial;
    auto expected_memory = initial_memory;
    const std::array sp_instructions{
        memory::unsigned_instruction(
            load.size_code, load.operation, 0, 31, 10),
        memory::unsigned_instruction(
            load.size_code, load.operation, 1, 31, 11),
    };
    apply_unsigned_oracle(
        initial, expected, initial_memory, expected_memory, physical_base,
        load, 0, 10);
    apply_unsigned_oracle(
        initial, expected, initial_memory, expected_memory, physical_base,
        load, 1, 11);
    check_success(
        harness, "jit-simd-run-sp-unsigned-state-exemption",
        sp_instructions, initial, expected, data_base, initial_memory,
        expected_memory);
  }
}

void ordinary_store_run_behavior(engine::EngineHarness &harness) {
  const auto &store = memory::single_variants[8];
  std::vector<std::byte> initial_memory(page_size);
  memory::fill_memory(initial_memory, 193);
  auto expected_memory = initial_memory;
  auto initial = memory::seeded_state(0xd900'0000'0000'0001ULL);
  initial.registers[27] = data_base + page_size / 4U;
  auto expected = initial;
  const std::array instructions{
      memory::unsigned_instruction(
          store.size_code, store.operation, 0, 27, 12),
      memory::unsigned_instruction(
          store.size_code, store.operation, 1, 27, 13),
  };
  apply_unsigned_oracle(
      initial, expected, initial_memory, expected_memory,
      initial.registers[27], store, 0, 12);
  apply_unsigned_oracle(
      initial, expected, initial_memory, expected_memory,
      initial.registers[27], store, 1, 13);
  check_success(
      harness, "jit-simd-run-ordinary-store-behavior", instructions, initial,
      expected, data_base, initial_memory, expected_memory);
}

struct Run final {
  ::uml::cpu::EngineEvent event{};
  std::size_t executed{};
};

[[nodiscard]] Run run_once(
    uml::cpu::arm64::JitExecutor &executor,
    uml::memory::StaticGuestMemory &guest_memory, unsigned long generation,
    arm64::CpuState &cpu) {
  engine::require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  const auto event =
      executor.run_block(cpu, guest_memory, generation, executed);
  executor.end_execution();
  return {event, executed};
}

void jit_local_monitor_survives_stores() {
  constexpr unsigned long generation = 9551;
  constexpr std::uint64_t base = 0xd8'0000;
  const auto &store = memory::single_variants[8];
  engine::JitBacking backing{2};
  backing.put32(
      0, memory::unsigned_instruction(
             store.size_code, store.operation, 0, 31, 12));
  backing.put32(
      4, memory::unsigned_instruction(
             store.size_code, store.operation, 1, 31, 13));
  backing.put32(8, system_call);
  engine::JitMapping mapping{generation, base, backing, 2};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory guest_memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.sp = base + page_size;
  cpu.exclusive_address = base + page_size + 128U;
  cpu.exclusive_size = 16;

  const auto run =
      run_once(executor, guest_memory, generation, cpu);
  engine::require(run.event == ::uml::cpu::EngineEvent::system_call);
  engine::require(run.executed == 3);
  // A store of the guest's own leaves the monitor standing.
  engine::require(cpu.exclusive_size == 16);
}

void remapped_page_uses_new_backing() {
  constexpr unsigned long generation = 9601;
  constexpr std::uint64_t base = 0xe0'0000;
  std::array<std::byte, 32> old_values{};
  std::array<std::byte, 32> new_values{};
  for (std::size_t index = 0; index < old_values.size(); ++index) {
    old_values[index] = static_cast<std::byte>(0x20U + index);
    new_values[index] = static_cast<std::byte>(0xa0U + index);
  }
  const auto &load = memory::single_variants[9];
  engine::JitBacking original{2};
  engine::JitBacking replacement{1};
  original.put32(
      0, memory::unsigned_instruction(
             load.size_code, load.operation, 0, 31, 1));
  original.put32(
      4, memory::unsigned_instruction(
             load.size_code, load.operation, 1, 31, 2));
  original.put32(8, system_call);
  original.put(page_size, old_values);
  replacement.put(0, new_values);
  engine::JitMapping mapping{generation, base, original, 2};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory guest_memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.sp = base + page_size;

  auto run = run_once(executor, guest_memory, generation, cpu);
  engine::require(run.event == ::uml::cpu::EngineEvent::system_call);
  engine::require(run.executed == 3);
  engine::require(std::equal(
      old_values.begin(), old_values.begin() + 16,
      cpu.vectors[1].bytes.begin()));
  engine::require(std::equal(
      old_values.begin() + 16, old_values.end(),
      cpu.vectors[2].bytes.begin()));

  mapping.replace_page(base + page_size, replacement);
  cpu.pc = base;
  cpu.vectors[1].bytes.fill(std::byte{});
  cpu.vectors[2].bytes.fill(std::byte{});
  run = run_once(executor, guest_memory, generation, cpu);
  engine::require(run.event == ::uml::cpu::EngineEvent::system_call);
  engine::require(run.executed == 3);
  engine::require(std::equal(
      new_values.begin(), new_values.begin() + 16,
      cpu.vectors[1].bytes.begin()));
  engine::require(std::equal(
      new_values.begin() + 16, new_values.end(),
      cpu.vectors[2].bytes.begin()));
}

enum class ReservationTarget : unsigned {
  first,
  second,
  nonoverlap,
};

void tracked_store_reservation_case(
    const memory::SingleVariant &variant, unsigned variant_index,
    ReservationTarget target) {
  const auto case_index =
      variant_index * 3U + static_cast<unsigned>(target);
  const auto generation = static_cast<unsigned long>(9700U + case_index);
  const auto base =
      std::uint64_t{0xf0'0000} + case_index * 4U * page_size;
  constexpr std::size_t first_offset = 32;
  constexpr std::size_t second_offset = 64;
  constexpr std::size_t nonoverlap_offset = 128;
  const auto reservation_offset =
      target == ReservationTarget::first
          ? first_offset + variant.size - 1U
      : target == ReservationTarget::second
          ? second_offset + variant.size - 1U
          : nonoverlap_offset;
  constexpr std::uint64_t exclusive_value = 0xc7U;
  std::array<std::byte, 16> first_value{};
  std::array<std::byte, 16> second_value{};
  for (std::size_t index = 0; index < first_value.size(); ++index) {
    first_value[index] =
        static_cast<std::byte>(0x31U + variant_index * 7U + index);
    second_value[index] =
        static_cast<std::byte>(0xb1U + variant_index * 5U + index);
  }

  engine::JitBacking backing{2};
  backing.put32(0, load_exclusive_byte(1, 5));
  backing.put32(4, system_call);
  backing.put32(
      8, memory::unsigned_instruction(
             variant.size_code, variant.operation,
             static_cast<unsigned>(first_offset / variant.size), 31, 2));
  backing.put32(
      12, memory::unsigned_instruction(
              variant.size_code, variant.operation,
              static_cast<unsigned>(second_offset / variant.size), 31, 3));
  backing.put32(16, system_call);
  backing.put32(20, store_exclusive_byte(6, 1, 4));
  backing.put32(24, system_call);
  std::array<std::byte, 16> zeros{};
  backing.put(page_size + first_offset, zeros);
  backing.put(page_size + second_offset, zeros);
  backing.put(page_size + nonoverlap_offset, zeros);
  engine::JitMapping mapping{generation, base, backing, 2};

  uml::cpu::arm64::JitExecutor first;
  uml::cpu::arm64::JitExecutor second;
  uml::memory::StaticGuestMemory shared_memory;
  arm64::CpuState first_cpu{};
  arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] =
      base + page_size + reservation_offset;
  first_cpu.registers[4] = exclusive_value;
  second_cpu.pc = base + 8U;
  second_cpu.sp = base + page_size;
  second_cpu.vectors[2].bytes = first_value;
  second_cpu.vectors[3].bytes = second_value;

  engine::require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  engine::require(first.run_block(
                      first_cpu, shared_memory, generation, executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(executed == 1);
  engine::require(first_cpu.exclusive_size == 1);

  engine::require(second.begin_execution(generation, second_cpu.pc));
  engine::require(second.run_block(
                      second_cpu, shared_memory, generation, executed) ==
                  ::uml::cpu::EngineEvent::system_call);
  engine::require(executed == 3);
  std::array<std::byte, 16> stored_first{};
  std::array<std::byte, 16> stored_second{};
  backing.get(page_size + first_offset, stored_first);
  backing.get(page_size + second_offset, stored_second);
  engine::require(std::equal(
      first_value.begin(),
      first_value.begin() + static_cast<std::ptrdiff_t>(variant.size),
      stored_first.begin()));
  engine::require(std::equal(
      second_value.begin(),
      second_value.begin() + static_cast<std::ptrdiff_t>(variant.size),
      stored_second.begin()));
  second.end_execution();

  first_cpu.pc = base + 20U;
  engine::require(first.run_block(
                      first_cpu, shared_memory, generation, executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(executed == 1);
  const bool overlaps = target != ReservationTarget::nonoverlap;
  engine::require(first_cpu.registers[6] == (overlaps ? 1U : 0U));
  engine::require(first_cpu.exclusive_size == 0);
  if (!overlaps) {
    std::array<std::byte, 16> nonoverlap{};
    backing.get(page_size + nonoverlap_offset, nonoverlap);
    engine::require(
        static_cast<unsigned>(nonoverlap[0]) == exclusive_value);
  }
  first.end_execution();
}

void tracked_store_runs_track_every_range() {
  constexpr std::array store_indices{0U, 2U, 4U, 6U, 8U};
  for (unsigned index = 0; index < store_indices.size(); ++index) {
    const auto &variant = memory::single_variants[store_indices[index]];
    for (const auto target : {
             ReservationTarget::first,
             ReservationTarget::second,
             ReservationTarget::nonoverlap,
         })
      tracked_store_reservation_case(variant, index, target);
  }
}

void straddling_runs_run_natively() {
  /* Each case's run straddles the first data page's end. */
  namespace straddle = arm64_jit_memory_run_straddle_test;
  using straddle::load_d;
  using straddle::load_q;
  using straddle::pattern;
  using straddle::pattern_x;
  using straddle::store_q;
  constexpr auto end = page_size;
  const auto guest = [](unsigned index) {
    return std::uint64_t{0xf0'0000} + index * 4U * page_size;
  };
  const auto with_base = [](const straddle::Case &test_case, unsigned base,
                            std::uint64_t offset) {
    arm64::CpuState cpu{};
    if (base == 31U)
      cpu.sp = straddle::data(test_case) + offset;
    else
      cpu.registers[base] = straddle::data(test_case) + offset;
    return cpu;
  };
  /* Whether V<reg> holds the `bytes` data bytes at `offset`, then zeros. */
  const auto holds = [](const arm64::CpuState &cpu, unsigned reg,
                        std::uint64_t offset, std::size_t bytes) {
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto want = index < bytes ? pattern(offset + index) : std::byte{};
      if (cpu.vectors[reg].bytes[index] != want)
        return false;
    }
    return true;
  };

  for (const unsigned base : {31U, 22U}) {
    const auto index = base == 31U ? 0U : 1U;
    const std::array code{load_q(1, base, 0), load_q(2, base, 1)};
    const straddle::Case test_case{
        .name = base == 31U ? "jit-simd-run-straddle-sp-q-load"
                            : "jit-simd-run-straddle-gpr-q-load",
        .generation = 9721U + index,
        .guest = guest(index),
        .run = code};
    straddle::check(test_case, with_base(test_case, base, end - 16U),
                    [&](const arm64::CpuState &cpu, auto) {
                      engine::require(holds(cpu, 1, end - 16U, 16) &&
                                      holds(cpu, 2, end, 16));
                    });
  }
  {
    const std::array code{load_d(1, 22, 1), load_d(2, 22, 0)};
    const straddle::Case test_case{.name = "jit-simd-run-straddle-d-load",
                                   .generation = 9723,
                                   .guest = guest(2),
                                   .run = code};
    straddle::check(test_case, with_base(test_case, 22, end - 8U),
                    [&](const arm64::CpuState &cpu, auto) {
                      engine::require(holds(cpu, 1, end, 8) &&
                                      holds(cpu, 2, end - 8U, 8));
                    });
  }
  {
    const std::array code{store_q(1, 31, 0), store_q(2, 31, 1)};
    const straddle::Case test_case{.name = "jit-simd-run-straddle-q-store",
                                   .generation = 9724,
                                   .guest = guest(3),
                                   .run = code,
                                   .store = true};
    auto cpu = with_base(test_case, 31, end - 16U);
    for (std::size_t index = 0; index < 16U; ++index) {
      cpu.vectors[1].bytes[index] = static_cast<std::byte>(0x40U + index);
      cpu.vectors[2].bytes[index] = static_cast<std::byte>(0xc0U + index);
    }
    straddle::check(test_case, cpu, [&](const arm64::CpuState &, auto read) {
      engine::require(read(end - 16U) == 0x4746'4544'4342'4140ULL &&
                      read(end - 8U) == 0x4f4e'4d4c'4b4a'4948ULL &&
                      read(end) == 0xc7c6'c5c4'c3c2'c1c0ULL &&
                      read(end + 8U) == 0xcfce'cdcc'cbca'c9c8ULL &&
                      read(end + 16U) == pattern_x(end + 16U));
    });
  }
  {
    /* The second access crosses the boundary itself. */
    const std::array code{load_q(1, 22, 0), load_q(2, 22, 1)};
    const straddle::Case test_case{
        .name = "jit-simd-run-straddle-crossing-access",
        .generation = 9725,
        .guest = guest(4),
        .run = code,
        .native = false};
    straddle::check(test_case, with_base(test_case, 22, end - 24U),
                    [&](const arm64::CpuState &cpu, auto) {
                      engine::require(holds(cpu, 1, end - 24U, 16) &&
                                      holds(cpu, 2, end - 8U, 16));
                    });
  }
}

} // namespace

int main() {
  engine::require(arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 3));
  engine::EngineHarness harness;
  every_valid_variant_for_sp_and_gpr(harness);
  every_vector_register(harness);
  lengths_two_through_ten(harness);
  exact_page_edges_and_runtime_crossing(harness);
  first_and_later_faults(harness);
  alignment_and_permission_faults(harness);
  dirty_prefix_is_preserved_on_cold_fallback(harness);
  tagged_state_fallback_is_exact(harness);
  ordinary_store_run_behavior(harness);
  straddling_runs_run_natively();
  const auto &counts = harness.counts();
  engine::require(counts.gadgets != 0);
  if (engine::host_supports(memory::single_native())) {
    engine::require(counts.jit == counts.gadgets);
    engine::require(counts.jit_skipped == 0);
    engine::require(counts.jit_skipped_host_feature == 0);
  } else {
    engine::require(counts.jit == 0);
    engine::require(counts.jit_skipped == counts.gadgets);
    engine::require(counts.jit_skipped_host_feature == counts.gadgets);
  }
  engine::require(counts.jit_unavailable == 0);
  harness.print_summary("arm64-jit-simd-memory-run-runtime");
  if (engine::host_supports(memory::single_native())) {
    jit_local_monitor_survives_stores();
    remapped_page_uses_new_backing();
    tracked_store_runs_track_every_range();
  }
  return 0;
}
