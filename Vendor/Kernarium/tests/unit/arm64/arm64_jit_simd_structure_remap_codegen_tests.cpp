// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/cpu_state.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint64_t code_base = 0x5050'0000ULL;
constexpr unsigned first_cached_vector = 16U;
constexpr unsigned last_cached_vector = 31U;
constexpr std::uint32_t structure_translated_fields =
    0x0080'0000U | (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t bitwise_register_fields =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t monitor_clear =
    0x3900'0000U |
    (static_cast<std::uint32_t>(
         offsetof(arm64::CpuState, exclusive_size))
     << 10U) |
    (15U << 5U) | 31U;

enum class AddressMode {
  no_offset,
  post_immediate,
  post_register,
};

constexpr std::array modes{
    AddressMode::no_offset,
    AddressMode::post_immediate,
    AddressMode::post_register,
};

struct SingleForm final {
  bool valid{};
  bool replicate{};
  unsigned register_count{};
  std::size_t element_size{};
};

using uml::testing::require;

[[nodiscard]] constexpr bool post_index(AddressMode mode) noexcept {
  return mode != AddressMode::no_offset;
}

[[nodiscard]] constexpr unsigned offset_register(AddressMode mode) noexcept {
  return mode == AddressMode::no_offset       ? 0U
         : mode == AddressMode::post_immediate ? 31U
                                                : 5U;
}

[[nodiscard]] constexpr std::uint32_t
multiple_instruction(bool full_vector, bool load, AddressMode mode,
                     unsigned opcode, unsigned size, unsigned base,
                     unsigned transfer) noexcept {
  return 0x0c00'0000U |
         (static_cast<unsigned>(full_vector) << 30U) |
         (static_cast<unsigned>(post_index(mode)) << 23U) |
         (static_cast<unsigned>(load) << 22U) |
         ((offset_register(mode) & 31U) << 16U) |
         ((opcode & 15U) << 12U) | ((size & 3U) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
single_instruction(bool full_vector, bool load, AddressMode mode,
                   unsigned extra_register, unsigned opcode,
                   unsigned scale_bit, unsigned size, unsigned base,
                   unsigned transfer) noexcept {
  return 0x0d00'0000U |
         (static_cast<unsigned>(full_vector) << 30U) |
         (static_cast<unsigned>(post_index(mode)) << 23U) |
         (static_cast<unsigned>(load) << 22U) |
         ((extra_register & 1U) << 21U) |
         ((offset_register(mode) & 31U) << 16U) |
         ((opcode & 7U) << 13U) | ((scale_bit & 1U) << 12U) |
         ((size & 3U) << 10U) | ((base & 31U) << 5U) |
         (transfer & 31U);
}

[[nodiscard]] constexpr SingleForm
decode_single(bool load, unsigned extra_register, unsigned opcode,
              unsigned scale_bit, unsigned size) noexcept {
  const unsigned scale = opcode >> 1U;
  if (scale == 3U) {
    if (!load || scale_bit != 0U)
      return {};
    return {
        .valid = true,
        .replicate = true,
        .register_count =
            opcode == 6U ? 1U + extra_register : 3U + extra_register,
        .element_size = std::size_t{1} << size,
    };
  }

  const unsigned count =
      (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
  if (scale == 0U)
    return {true, false, count, 1U};
  if (scale == 1U) {
    if ((size & 1U) != 0U)
      return {};
    return {true, false, count, 2U};
  }
  if ((size & 2U) != 0U ||
      ((size & 1U) != 0U && scale_bit != 0U))
    return {};
  return {true, false, count, (size & 1U) != 0U ? 8U : 4U};
}

[[nodiscard]] constexpr std::uint32_t
bitwise(unsigned operation, unsigned destination, unsigned left,
        unsigned right) noexcept {
  return 0x4e20'1c00U | ((operation & 3U) << 22U) |
         ((operation & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled,
         std::size_t expected_guest) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete structure-memory program");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

[[nodiscard]] bool
is_cpu_state_vector_load(std::uint32_t instruction) noexcept {
  return (instruction & 0xffc0'03e0U) == 0x3dc0'01e0U;
}

[[nodiscard]] bool
is_cpu_state_vector_store(std::uint32_t instruction) noexcept {
  return (instruction & 0xffc0'03e0U) == 0x3d80'01e0U;
}

[[nodiscard]] std::size_t
count_if(std::span<const std::uint32_t> words,
         bool (*predicate)(std::uint32_t)) {
  return static_cast<std::size_t>(
      std::count_if(words.begin(), words.end(), predicate));
}

[[nodiscard]] std::size_t
count_word(std::span<const std::uint32_t> words, std::uint32_t value) {
  return static_cast<std::size_t>(
      std::count(words.begin(), words.end(), value));
}

[[nodiscard]] std::optional<std::uint32_t>
find_structure_native(std::span<const std::uint32_t> words,
                      std::uint32_t guest) {
  const auto expected = guest & ~structure_translated_fields;
  std::optional<std::uint32_t> result;
  for (const auto word : words) {
    if ((word & ~((31U << 5U) | 31U)) != expected ||
        ((word >> 5U) & 31U) != 8U)
      continue;
    require(!result.has_value(),
            "JIT emitted a structure-memory instruction more than once");
    result = word;
  }
  return result;
}

[[nodiscard]] std::optional<std::uint32_t>
find_bitwise_native(std::span<const std::uint32_t> words,
                    std::uint32_t guest) {
  std::optional<std::uint32_t> result;
  for (const auto word : words) {
    if ((word & ~bitwise_register_fields) !=
        (guest & ~bitwise_register_fields))
      continue;
    require(!result.has_value(),
            "JIT emitted a vector producer/consumer more than once");
    result = word;
  }
  return result;
}

void every_full_one_register_form_is_direct() {
  std::uint64_t pc = code_base;
  for (const bool full_vector : {false, true})
    for (const bool load : {false, true})
      for (unsigned size = 0; size < 4U; ++size)
        for (const auto mode : modes) {
          const auto instruction = multiple_instruction(
              full_vector, load, mode, 7U, size, 2U, 29U);
          const std::array guest{instruction};
          const auto compiled = jit_test::compile_block(pc, guest);
          pc += 0x100U;
          const auto hot = hot_code(compiled, guest.size());
          const auto native = find_structure_native(hot, instruction);
          require(native.has_value(),
                  "full one-register form was not emitted natively");
          require((*native & 31U) >= first_cached_vector &&
                      (*native & 31U) <= last_cached_vector,
                  "full one-register form did not name a resident vector");
          require(count_if(hot, &is_cpu_state_vector_load) ==
                      (load ? 0U : 1U),
                  "full one-register form imported the wrong vector");
          require(count_if(hot, &is_cpu_state_vector_store) ==
                      (load ? 1U : 0U),
                  "full one-register form exported the wrong vector");
          require(count_word(hot, monitor_clear) == 0U,
                  "full one-register form cleared the monitor");
          require(compiled.memory_write_mask == (load ? 0U : 1U) &&
                      compiled.direct_memory_write_mask ==
                          (load ? 0U : 1U),
                  "full one-register form has the wrong write masks");
        }
}

void every_lane_one_register_form_is_direct() {
  std::uint64_t pc = code_base + 0x10'000U;
  for (const bool full_vector : {false, true})
    for (const bool load : {false, true})
      for (unsigned opcode = 0; opcode < 6U; ++opcode)
        for (unsigned scale_bit = 0; scale_bit < 2U; ++scale_bit)
          for (unsigned size = 0; size < 4U; ++size) {
            const auto form =
                decode_single(load, 0U, opcode, scale_bit, size);
            if (!form.valid || form.replicate ||
                form.register_count != 1U)
              continue;
            for (const auto mode : modes) {
              const auto instruction = single_instruction(
                  full_vector, load, mode, 0U, opcode, scale_bit,
                  size, 2U, 30U);
              const std::array guest{instruction};
              const auto compiled = jit_test::compile_block(pc, guest);
              pc += 0x100U;
              const auto hot = hot_code(compiled, guest.size());
              const auto native = find_structure_native(hot, instruction);
              require(native.has_value(),
                      "lane one-register form was not emitted natively");
              require((*native & 31U) >= first_cached_vector &&
                          (*native & 31U) <= last_cached_vector,
                      "lane form did not name a resident vector");
              require(count_if(hot, &is_cpu_state_vector_load) == 1U,
                      "lane form did not import its source/old destination");
              require(count_if(hot, &is_cpu_state_vector_store) ==
                          (load ? 1U : 0U),
                      "lane form exported the wrong vector state");
              require(count_word(hot, monitor_clear) == 0U,
                      "lane form cleared the monitor");
              require(compiled.memory_write_mask == (load ? 0U : 1U) &&
                          compiled.direct_memory_write_mask ==
                              (load ? 0U : 1U),
                      "lane one-register form has the wrong write masks");
            }
          }
}

void every_replicate_one_register_form_is_direct() {
  std::uint64_t pc = code_base + 0x30'000U;
  for (const bool full_vector : {false, true})
    for (unsigned size = 0; size < 4U; ++size)
      for (const auto mode : modes) {
        const auto form = decode_single(true, 0U, 6U, 0U, size);
        require(form.valid && form.replicate &&
                    form.register_count == 1U,
                "test encoded a non-replicate form");
        const auto instruction = single_instruction(
            full_vector, true, mode, 0U, 6U, 0U, size, 2U, 31U);
        const std::array guest{instruction};
        const auto compiled = jit_test::compile_block(pc, guest);
        pc += 0x100U;
        const auto hot = hot_code(compiled, guest.size());
        const auto native = find_structure_native(hot, instruction);
        require(native.has_value(),
                "replicate one-register form was not emitted natively");
        require((*native & 31U) >= first_cached_vector &&
                    (*native & 31U) <= last_cached_vector,
                "replicate form did not name a resident vector");
        require(count_if(hot, &is_cpu_state_vector_load) == 0U,
                "replicate form unnecessarily imported its destination");
        require(count_if(hot, &is_cpu_state_vector_store) == 1U,
                "replicate form did not publish its resident result");
        require(compiled.memory_write_mask == 0U &&
                    compiled.direct_memory_write_mask == 0U,
                "replicate load unexpectedly marks a memory write");
      }
}

void producer_and_consumer_reuse_the_same_binding() {
  const auto check = [](std::uint32_t memory, std::uint64_t pc) {
    constexpr auto producer = bitwise(4U, 29U, 1U, 2U);
    constexpr auto consumer = bitwise(2U, 30U, 29U, 3U);
    const std::array guest{producer, memory, consumer};
    const auto compiled = jit_test::compile_block(pc, guest);
    const auto hot = hot_code(compiled, guest.size());
    const auto native_producer = find_bitwise_native(hot, producer);
    const auto native_memory = find_structure_native(hot, memory);
    const auto native_consumer = find_bitwise_native(hot, consumer);
    require(native_producer.has_value() && native_memory.has_value() &&
                native_consumer.has_value(),
            "resident structure dependency chain is incomplete");
    const auto producer_host = *native_producer & 31U;
    const auto memory_host = *native_memory & 31U;
    const auto consumer_host = (*native_consumer >> 5U) & 31U;
    require(producer_host == memory_host &&
                memory_host == consumer_host,
            "structure operation staged a resident producer/consumer");
    require(memory_host >= first_cached_vector &&
                memory_host <= last_cached_vector,
            "structure dependency escaped the resident vector cache");
  };

  check(multiple_instruction(true, false, AddressMode::no_offset,
                             7U, 0U, 2U, 29U),
        code_base + 0x40'000U);
  check(multiple_instruction(true, true, AddressMode::post_immediate,
                             7U, 1U, 2U, 29U),
        code_base + 0x41'000U);
  check(single_instruction(true, false, AddressMode::post_register,
                           0U, 4U, 0U, 0U, 2U, 29U),
        code_base + 0x42'000U);
  check(single_instruction(true, true, AddressMode::no_offset,
                           0U, 4U, 1U, 0U, 2U, 29U),
        code_base + 0x43'000U);
  check(single_instruction(true, true, AddressMode::post_immediate,
                           0U, 6U, 0U, 3U, 2U, 29U),
        code_base + 0x44'000U);
}

constexpr std::array cache_fillers{
    bitwise(4U, 0U, 1U, 2U),    bitwise(4U, 3U, 4U, 5U),
    bitwise(4U, 6U, 7U, 8U),    bitwise(4U, 9U, 10U, 11U),
    bitwise(4U, 12U, 13U, 14U), bitwise(4U, 15U, 0U, 1U),
    bitwise(4U, 16U, 17U, 0U),
};

void full_cache_uses_the_exact_fallback() {
  const auto baseline = jit_test::compile_block(
      code_base + 0x50'000U, cache_fillers);
  const auto baseline_hot =
      hot_code(baseline, cache_fillers.size());
  const auto baseline_imports =
      count_if(baseline_hot, &is_cpu_state_vector_load);
  const auto baseline_exports =
      count_if(baseline_hot, &is_cpu_state_vector_store);

  const auto check = [&](std::uint32_t target,
                         std::size_t added_imports,
                         std::size_t added_exports,
                         std::uint64_t pc) {
    std::array<std::uint32_t, cache_fillers.size() + 1U> guest{};
    std::copy(cache_fillers.begin(), cache_fillers.end(), guest.begin());
    guest.back() = target;
    const auto compiled = jit_test::compile_block(pc, guest);
    const auto hot = hot_code(compiled, guest.size());
    const auto native = find_structure_native(hot, target);
    require(native.has_value(),
            "full-cache structure fallback did not emit natively");
    require((*native & 31U) == 0U,
            "full-cache structure fallback did not use v0");
    require(count_if(hot, &is_cpu_state_vector_load) ==
                baseline_imports + added_imports,
            "full-cache structure fallback imported the wrong state");
    require(count_if(hot, &is_cpu_state_vector_store) ==
                baseline_exports + added_exports,
            "full-cache structure fallback exported the wrong state");
  };

  check(multiple_instruction(true, false, AddressMode::no_offset,
                             7U, 0U, 2U, 31U),
        1U, 0U, code_base + 0x51'000U);
  check(multiple_instruction(true, true, AddressMode::no_offset,
                             7U, 0U, 2U, 31U),
        0U, 1U, code_base + 0x52'000U);
  check(single_instruction(true, true, AddressMode::no_offset,
                           0U, 4U, 0U, 0U, 2U, 31U),
        1U, 1U, code_base + 0x53'000U);
  check(single_instruction(true, true, AddressMode::no_offset,
                           0U, 6U, 0U, 3U, 2U, 31U),
        0U, 1U, code_base + 0x54'000U);
}

void high_and_wrapping_guest_registers_keep_distinct_bindings() {
  const auto first = multiple_instruction(
      true, true, AddressMode::no_offset, 7U, 0U, 2U, 31U);
  const auto second = multiple_instruction(
      true, true, AddressMode::no_offset, 7U, 1U, 3U, 0U);
  const std::array guest{first, second};
  const auto compiled =
      jit_test::compile_block(code_base + 0x60'000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto first_native = find_structure_native(hot, first);
  const auto second_native = find_structure_native(hot, second);
  require(first_native.has_value() && second_native.has_value(),
          "high/wrapping structure sequence is incomplete");
  require((*first_native & 31U) >= first_cached_vector &&
              (*first_native & 31U) <= last_cached_vector &&
              (*second_native & 31U) >= first_cached_vector &&
              (*second_native & 31U) <= last_cached_vector,
          "high/wrapping guest vector escaped the resident cache");
  require((*first_native & 31U) != (*second_native & 31U),
          "V31 and V0 incorrectly share one resident binding");
}

void multi_register_wrapping_forms_remain_staged() {
  for (const bool load : {false, true}) {
    const auto instruction = multiple_instruction(
        true, load, AddressMode::no_offset, 10U, 0U, 2U, 31U);
    const std::array guest{instruction};
    const auto compiled = jit_test::compile_block(
        code_base + 0x61'000U + static_cast<unsigned>(load) * 0x100U,
        guest);
    const auto hot = hot_code(compiled, guest.size());
    const auto native = find_structure_native(hot, instruction);
    require(native.has_value(),
            "two-register wrapping structure form was not emitted");
    require((*native & 31U) == 0U,
            "two-register structure form unexpectedly used one binding");
    require(count_if(hot, &is_cpu_state_vector_load) ==
                (load ? 0U : 2U),
            "two-register wrapping store imported the wrong vectors");
    require(count_if(hot, &is_cpu_state_vector_store) ==
                (load ? 2U : 0U),
            "two-register wrapping load exported the wrong vectors");
  }
}

} // namespace

int main() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd) {
    std::puts("arm64-jit-simd-structure-remap-codegen: skipped "
              "(host lacks Advanced SIMD)");
    return 0;
  }

  every_full_one_register_form_is_direct();
  every_lane_one_register_form_is_direct();
  every_replicate_one_register_form_is_direct();
  producer_and_consumer_reuse_the_same_binding();
  full_cache_uses_the_exact_fallback();
  high_and_wrapping_guest_registers_keep_distinct_bindings();
  multi_register_wrapping_forms_remain_staged();
  std::puts("arm64-jit-simd-structure-remap-codegen: ok");
  return 0;
}
