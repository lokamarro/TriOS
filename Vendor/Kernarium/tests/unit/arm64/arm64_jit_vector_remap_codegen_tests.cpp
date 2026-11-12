// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_vector_remap_test_support.hpp"
#include "platform/apple/host_features.hpp"

namespace {

void vector_dependency_chain_is_direct_and_resident() {
  /*
   * These unrestricted-register 128-bit bitwise forms are part of baseline
   * Advanced SIMD. The two long-lived inputs also prevent a trivial
   * single-temporary lowering from satisfying the dependency checks.
   */
  constexpr std::array guest{
      bitwise(4, 0, 1, 2), // eor v0.16b, v1.16b, v2.16b
      bitwise(2, 3, 0, 2), // orr v3.16b, v0.16b, v2.16b
      bitwise(0, 4, 3, 1), // and v4.16b, v3.16b, v1.16b
      bitwise(1, 5, 4, 2), // bic v5.16b, v4.16b, v2.16b
      bitwise(3, 6, 5, 1), // orn v6.16b, v5.16b, v1.16b
  };

  const auto compiled = jit_test::compile_block(code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete vector dependency chain");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated vector code");
  require(compiled.hot_words >= guest.size(),
          "JIT vector hot path is shorter than the guest operation run");

  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found, "JIT emitted the vector dependency chain more than once");
    found = true;
  }
  require(found,
          "JIT did not emit one consecutive native word per vector operation");
}

void specialized_vector_dependency_chain_is_direct_and_resident() {
  /*
   * The byte-lane form is part of baseline Advanced SIMD, so this code-shape
   * regression remains useful even when optional host features are absent.
   */
  constexpr std::array guest{
      bitwise(4, 5, 1, 2),
      polynomial_multiply(6, 5, 2),
      bitwise(2, 7, 6, 1),
  };

  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the specialized vector dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found,
            "JIT emitted the specialized vector chain more than once");
    found = true;
  }
  require(found,
          "specialized vector operation staged through fixed registers");
}

void require_direct_three_vector_operation(std::string_view name,
                                           std::uint32_t operation,
                                           std::uint64_t pc) {
  constexpr unsigned destination_guest = 0U;
  constexpr unsigned left_guest = 1U;
  constexpr unsigned right_guest = 2U;
  const std::array guest{
      bitwise(4, destination_guest, 3, 4),
      bitwise(4, left_guest, 5, 6),
      bitwise(4, right_guest, 7, 8),
      operation | (right_guest << 16U) | (left_guest << 5U) |
          destination_guest,
      bitwise(2, 9, destination_guest, 10),
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile a direct vector dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 3> producers{};
  std::size_t producer_count{};
  std::optional<std::uint32_t> native_operation;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask)) {
      require(producer_count < producers.size(),
              "JIT emitted an unexpected vector-chain producer");
      producers[producer_count++] = word;
    }
    if ((word & ~register_mask) == (guest[3] & ~register_mask)) {
      require(!native_operation.has_value(),
              "JIT emitted a vector dependency operation twice");
      native_operation = word;
    }
    if ((word & ~register_mask) == (guest[4] & ~register_mask)) {
      require(!consumer.has_value(),
              "JIT emitted a vector dependency consumer twice");
      consumer = word;
    }
  }
  if (producer_count != producers.size() ||
      !native_operation.has_value() || !consumer.has_value()) {
    std::fprintf(stderr, "%.*s is missing a direct native operation\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  require(destination(producers[0]) == destination(*native_operation),
          "vector destination was staged through a fixed register");
  require(destination(producers[1]) == left(*native_operation),
          "vector first source was staged through a fixed register");
  require(destination(producers[2]) == right(*native_operation),
          "vector second source was staged through a fixed register");
  require(destination(*native_operation) == left(*consumer),
          "vector result was not retained for its consumer");
  require(destination(*native_operation) >= first_cached_vector &&
              destination(*native_operation) <= last_cached_vector &&
              left(*native_operation) >= first_cached_vector &&
              left(*native_operation) <= last_cached_vector &&
              right(*native_operation) >= first_cached_vector &&
              right(*native_operation) <= last_cached_vector,
          "vector operation escaped the resident vector cache");
}

void remaining_floating_families_name_resident_vectors_directly() {
  require_direct_three_vector_operation(
      "vector horizontal pairwise add", 0x6e20'd400U,
      code_base + 0x20'000U);
  require_direct_three_vector_operation(
      "scalar reciprocal step", 0x5e20'fc00U,
      code_base + 0x21'000U);
}

void remaining_simd_arithmetic_families_name_resident_vectors_directly() {
  const auto &host = ::uml::platform::apple::probed_host_capabilities();
  if (!host.advanced_simd)
    return;

  require_direct_three_vector_operation(
      "widening accumulator", 0x0e20'5000U,
      code_base + 0x29'000U);
  require_direct_three_vector_operation(
      "word indexed integer accumulator", 0x6f80'0000U,
      code_base + 0x2a'000U);
  require_direct_three_vector_operation(
      "table lookup", 0x4e00'0000U,
      code_base + 0x2b'000U);
  require_direct_three_vector_operation(
      "vector permute", 0x4e00'2800U,
      code_base + 0x2c'000U);

  if (host.floating_point)
    require_direct_three_vector_operation(
        "word indexed floating accumulator", 0x4f80'1000U,
        code_base + 0x2d'000U);
  if (host.has(::uml::cpu::ArmFeature::bf16)) {
    require_direct_three_vector_operation(
        "bfloat vector dot accumulator", 0x6e40'fc00U,
        code_base + 0x2e'000U);
    require_direct_three_vector_operation(
        "bfloat indexed dot accumulator", 0x4f40'f000U,
        code_base + 0x2f'000U);
    require_direct_three_vector_operation(
        "bfloat vector multiply-long accumulator", 0x6ec0'fc00U,
        code_base + 0x30'000U);
    require_direct_three_vector_operation(
        "bfloat matrix accumulator", 0x6e40'ec00U,
        code_base + 0x31'000U);
  }
  if (host.has(::uml::cpu::ArmFeature::fhm))
    require_direct_three_vector_operation(
        "half vector fused-long accumulator", 0x4e20'ec00U,
        code_base + 0x34'000U);
  if (host.has(::uml::cpu::ArmFeature::dotprod)) {
    require_direct_three_vector_operation(
        "integer vector dot accumulator", 0x4e80'9400U,
        code_base + 0x36'000U);
    require_direct_three_vector_operation(
        "integer indexed dot accumulator", 0x4f80'e000U,
        code_base + 0x37'000U);
  }
  if (host.has(::uml::cpu::ArmFeature::i8mm))
    require_direct_three_vector_operation(
        "integer matrix accumulator", 0x4e80'a400U,
        code_base + 0x38'000U);
}

void require_restricted_element_operation(std::string_view name,
                                          std::uint32_t operation,
                                          std::uint64_t pc) {
  constexpr std::uint32_t restricted_register_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr unsigned destination_guest = 0U;
  constexpr unsigned left_guest = 1U;
  constexpr unsigned element_guest = 2U;
  const std::array guest{
      bitwise(4, destination_guest, 3U, 4U),
      bitwise(4, left_guest, 5U, 6U),
      bitwise(4, element_guest, 7U, 8U),
      operation | (element_guest << 16U) | (left_guest << 5U) |
          destination_guest,
      bitwise(2, 9U, destination_guest, 10U),
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile a restricted-element dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 3> producers{};
  std::size_t producer_count{};
  std::optional<std::uint32_t> native_operation;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask)) {
      require(producer_count < producers.size(),
              "JIT emitted an unexpected restricted-chain producer");
      producers[producer_count++] = word;
    }
    if ((word & ~restricted_register_mask) ==
        (guest[3] & ~restricted_register_mask)) {
      require(!native_operation.has_value(),
              "JIT emitted a restricted-element operation twice");
      native_operation = word;
    }
    if ((word & ~register_mask) == (guest[4] & ~register_mask))
      consumer = word;
  }
  if (producer_count != producers.size() ||
      !native_operation.has_value() || !consumer.has_value()) {
    std::fprintf(stderr,
                 "%.*s is missing its restricted native operation\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  require(destination(producers[0]) == destination(*native_operation),
          "restricted operation did not use its resident destination");
  require(destination(producers[1]) == left(*native_operation),
          "restricted operation did not use its resident first source");
  require(destination(producers[2]) == right(*native_operation),
          "restricted element did not use its pinned low-register binding");
  require(right(*native_operation) == 6U ||
              right(*native_operation) == 7U,
          "restricted element escaped the q6/q7 resident bank");
  require(destination(*native_operation) == left(*consumer),
          "restricted operation result was not retained for its consumer");
}

void restricted_element_families_use_the_low_resident_bank() {
  const auto &host = ::uml::platform::apple::probed_host_capabilities();
  if (!host.advanced_simd)
    return;

  require_restricted_element_operation(
      "halfword indexed integer accumulator", 0x6f40'0000U,
      code_base + 0x3a'000U);
  if (host.fp16 && host.floating_point)
    require_restricted_element_operation(
        "half indexed floating accumulator", 0x4f00'1000U,
        code_base + 0x3b'000U);
  if (host.has(::uml::cpu::ArmFeature::fhm))
    require_restricted_element_operation(
        "half indexed fused-long accumulator", 0x4f80'0000U,
        code_base + 0x3c'000U);
  if (host.has(::uml::cpu::ArmFeature::bf16))
    require_restricted_element_operation(
        "bfloat indexed multiply-long accumulator", 0x4fc0'f000U,
        code_base + 0x3d'000U);
}

void constrained_frequency_selects_two_stable_pins() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr std::uint32_t operand_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  /*
   * v7 is most frequent and must take q6. v9 and v8 tie; v9 appears first
   * and must take q7. The unpinned v8 identity remains on the q2 fallback
   * without shortening the block.
   */
  constexpr std::array guest{
      indexed_mla(0U, 1U, 9U),
      indexed_mla(0U, 1U, 8U),
      indexed_mla(0U, 1U, 7U),
      indexed_mla(0U, 1U, 8U),
      indexed_mla(0U, 1U, 9U),
      indexed_mla(0U, 1U, 7U),
      indexed_mla(0U, 1U, 7U),
  };
  constexpr std::array expected_hosts{7U, 2U, 6U, 2U, 7U, 6U, 6U};
  const auto compiled =
      jit_test::compile_block(code_base + 0x4c'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "third constrained identity shortened the JIT block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::size_t operation{};
  for (const auto word : hot) {
    if (operation == guest.size() ||
        (word & ~operand_mask) != (guest[operation] & ~operand_mask))
      continue;
    require(right(word) == expected_hosts[operation],
            "constrained source frequency/tie selection changed");
    ++operation;
  }
  require(operation == guest.size(),
          "frequency-selection sequence omitted a native operation");
}

void constrained_pin_is_visible_to_an_earlier_producer() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr auto producer = bitwise(4U, 9U, 1U, 2U);
  constexpr auto constrained =
      0x6f40'0000U | (9U << 16U) | (3U << 5U);
  constexpr auto consumer = bitwise(2U, 4U, 9U, 5U);
  constexpr std::array guest{producer, constrained, consumer};
  const auto compiled =
      jit_test::compile_block(code_base + 0x4e'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "prebound constrained sequence did not remain one block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> native_producer;
  std::optional<std::uint32_t> native_constrained;
  std::optional<std::uint32_t> native_consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (producer & ~register_mask))
      native_producer = word;
    if ((word & ~((15U << 16U) | (31U << 5U) | 31U)) ==
        (constrained & ~((15U << 16U) | (31U << 5U) | 31U)))
      native_constrained = word;
    if ((word & ~register_mask) == (consumer & ~register_mask))
      native_consumer = word;
  }
  require(native_producer.has_value() &&
              native_constrained.has_value() &&
              native_consumer.has_value(),
          "prebound constrained sequence omitted a native operation");
  require(destination(*native_producer) == 6U,
          "translation did not prebind the constrained identity in q6");
  require(right(*native_constrained) == 6U,
          "constrained use did not reuse the q6 binding");
  require(left(*native_consumer) == 6U,
          "ordinary consumer did not reuse the q6 binding");
}

void third_constrained_identity_uses_q2_when_high_bank_is_full() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr std::uint32_t constrained_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  constexpr std::array fill_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 16U, 17U, 31U,
  };
  std::array<std::uint32_t, 2U + fill_guests.size() + 1U> guest{};
  guest[0] = indexed_mla(13U, 13U, 13U);
  guest[1] = indexed_mla(14U, 14U, 14U);
  for (std::size_t index = 0; index < fill_guests.size(); ++index)
    guest[index + 2U] =
        modified_immediate(false, false, index + 1U, fill_guests[index]);
  const auto target = indexed_mla(31U, 31U, 15U);
  guest.back() = target;

  const auto compiled =
      jit_test::compile_block(code_base + 0x50'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "third constrained identity split a full-cache block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> native;
  for (const auto word : hot)
    if ((word & ~constrained_mask) == (target & ~constrained_mask))
      native = word;
  require(native.has_value(),
          "full-cache third constrained operation was not emitted");
  require(left(*native) == 31U && destination(*native) == 31U &&
              right(*native) == 2U,
          "third constrained identity did not use the q2 fallback");
}

void constrained_planning_stops_at_a_terminal() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr std::uint32_t constrained_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  constexpr auto before = indexed_mla(0U, 1U, 9U);
  constexpr auto after = indexed_mla(2U, 3U, 8U);
  constexpr std::array guest{
      before, 0xd400'0001U, after, after, after, after,
  };
  const auto compiled =
      jit_test::compile_block(code_base + 0x51'000U, guest);
  require(compiled.guest_instructions == 2U,
          "terminal did not bound the translated constrained prefix");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> native;
  for (const auto word : hot)
    if ((word & ~constrained_mask) == (before & ~constrained_mask))
      native = word;
  require(native.has_value(),
          "pre-terminal constrained operation was not emitted");
  require(right(*native) == 6U,
          "post-terminal frequency changed the translated prefix binding");
}

void future_constrained_word_does_not_expand_a_terminal_prefix() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;

  constexpr auto ordinary = bitwise(4U, 9U, 1U, 2U);
  constexpr std::uint32_t system_call = 0xd400'0001U;
  constexpr auto future_constrained =
      0x6f40'0000U | (9U << 16U) | (3U << 5U);
  constexpr std::array prefix{ordinary, system_call};
  constexpr std::array fetched{ordinary, system_call, future_constrained};
  const auto expected =
      jit_test::compile_block(code_base + 0x4f'000U, prefix);
  const auto actual =
      jit_test::compile_block(code_base + 0x4f'000U, fetched);
  require(expected.guest_instructions == prefix.size() &&
              actual.guest_instructions == prefix.size(),
          "system-call prefix retired an unexpected instruction count");
  require(actual.words.size() == expected.words.size() &&
              actual.hot_words == expected.hot_words &&
              actual.internal_entry_word == expected.internal_entry_word &&
              actual.memory_write_mask == expected.memory_write_mask &&
              actual.direct_memory_write_mask ==
                  expected.direct_memory_write_mask &&
              actual.accesses_fp_state == expected.accesses_fp_state &&
              actual.native_fp == expected.native_fp,
          "future constrained word expanded the terminal prefix");
}

void require_ordinary_vector_memory_chain(
    std::span<const std::uint32_t> guest, std::uint64_t pc) {
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete vector-memory chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> producer;
  std::optional<std::uint32_t> consumer;
  std::optional<std::uint32_t> load;
  std::array<std::uint32_t, 2> stores{};
  std::size_t store_count = 0;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask))
      producer = word;
    if ((word & ~register_mask) == (guest[4] & ~register_mask))
      consumer = word;
    if ((word & ~31U) == 0x3d80'0100U) {
      require(store_count < stores.size(),
              "JIT emitted an unexpected direct vector store");
      stores[store_count++] = word;
    }
    if ((word & ~31U) == 0x3dc0'0100U) {
      require(!load.has_value(),
              "JIT emitted an unexpected direct vector load");
      load = word;
    }
  }

  require(producer.has_value(),
          "JIT did not emit the resident vector producer");
  require(consumer.has_value(),
          "JIT did not emit the resident vector consumer");
  require(store_count == stores.size(),
          "JIT did not emit both direct vector stores");
  require(load.has_value(), "JIT did not emit the direct vector load");

  const auto loaded_host = destination(*load);
  require(destination(stores[0]) == destination(*producer),
          "resident vector producer was staged before its store");
  require(loaded_host == destination(stores[1]),
          "loaded vector was staged before its following store");
  require(loaded_host == left(*consumer),
          "loaded vector was staged before its dependent operation");
  require(right(*producer) == right(*consumer),
          "live vector input changed host binding across memory operations");
  require(destination(stores[0]) >= first_cached_vector &&
              destination(stores[0]) <= last_cached_vector &&
              loaded_host >= first_cached_vector &&
              loaded_host <= last_cached_vector,
          "ordinary vector memory data operand escaped the resident cache");
}

void ordinary_memory_names_resident_vector_registers_directly() {
  constexpr std::array unsigned_guest{
      bitwise(4, 5, 1, 2),                  // eor v5.16b, v1.16b, v2.16b
      vector_unsigned_memory(false, 10, 5), // str q5, [x10]
      vector_unsigned_memory(true, 10, 6),  // ldr q6, [x10]
      vector_unsigned_memory(false, 10, 6), // str q6, [x10]
      bitwise(2, 7, 6, 2),                  // orr v7.16b, v6.16b, v2.16b
  };
  require_ordinary_vector_memory_chain(unsigned_guest,
                                       code_base + 0x2000U);

  constexpr std::array unscaled_guest{
      bitwise(4, 5, 1, 2), // eor v5.16b, v1.16b, v2.16b
      0x3c80'0145U,         // stur q5, [x10]
      0x3cc0'0146U,         // ldur q6, [x10]
      0x3c80'0146U,         // stur q6, [x10]
      bitwise(2, 7, 6, 2), // orr v7.16b, v6.16b, v2.16b
  };
  require_ordinary_vector_memory_chain(unscaled_guest,
                                       code_base + 0x3000U);
}

void literal_load_names_its_resident_vector_directly() {
  constexpr std::array guest{
      0x9c00'0005U,         // ldr q5, literal
      bitwise(2, 6, 5, 2), // orr v6.16b, v5.16b, v2.16b
  };
  const auto compiled = jit_test::compile_block(code_base + 0x4000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the vector literal-load dependency");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> load;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~31U) == 0x3dc0'0100U)
      load = word;
    if ((word & ~register_mask) == (guest[1] & ~register_mask))
      consumer = word;
  }
  require(load.has_value(), "JIT did not emit the native vector literal load");
  require(consumer.has_value(),
          "JIT did not emit the vector literal load's cached consumer");
  require(destination(*load) == left(*consumer),
          "vector literal load was staged before its dependent operation");
  require(destination(*load) >= first_cached_vector &&
              destination(*load) <= last_cached_vector,
          "vector literal load destination escaped the resident cache");
}


} // namespace

void run_vector_remap_pressure_tests();

int main() {
  vector_dependency_chain_is_direct_and_resident();
  specialized_vector_dependency_chain_is_direct_and_resident();
  remaining_floating_families_name_resident_vectors_directly();
  remaining_simd_arithmetic_families_name_resident_vectors_directly();
  restricted_element_families_use_the_low_resident_bank();
  constrained_frequency_selects_two_stable_pins();
  constrained_pin_is_visible_to_an_earlier_producer();
  third_constrained_identity_uses_q2_when_high_bank_is_full();
  constrained_planning_stops_at_a_terminal();
  future_constrained_word_does_not_expand_a_terminal_prefix();
  ordinary_memory_names_resident_vector_registers_directly();
  literal_load_names_its_resident_vector_directly();
  run_vector_remap_pressure_tests();
  return 0;
}
