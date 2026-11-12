// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_vector_remap_test_support.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "platform/apple/host_features.hpp"

namespace {

[[nodiscard]] constexpr bool
is_cpu_state_vector_load(std::uint32_t instruction) noexcept {
  /*
   * LDR Qt, [x15, #imm] is the only generated instruction that imports a
   * guest vector from CpuState. Ignore Rt and the scaled immediate.
   */
  return (instruction & 0xffc0'03e0U) == 0x3dc0'01e0U;
}

[[nodiscard]] std::optional<std::uint32_t>
find_native_operation(std::span<const std::uint32_t> hot,
                      std::uint32_t guest,
                      std::uint32_t operand_mask) {
  std::optional<std::uint32_t> result;
  for (const auto word : hot) {
    if ((word & ~operand_mask) != (guest & ~operand_mask))
      continue;
    require(!result.has_value(),
            "JIT emitted a checked native operation more than once");
    result = word;
  }
  return result;
}

void unrestricted_low_pressure_shape_is_unchanged() {
  constexpr std::array guest{bitwise(4U, 0U, 1U, 2U)};
  const auto compiled =
      jit_test::compile_block(code_base + 0x52'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "low-pressure unrestricted operation did not compile");
  require(compiled.hot_words == 22U && compiled.words.size() == 24U,
          "low-pressure unrestricted code size changed");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  const auto native =
      find_native_operation(hot, guest.front(), register_mask);
  require(native.has_value(),
          "low-pressure unrestricted operation omitted its native word");
  require(left(*native) == 16U && right(*native) == 17U &&
              destination(*native) == 18U,
          "unrestricted low-pressure register order changed");
  require(std::count_if(hot.begin(), hot.end(),
                        is_cpu_state_vector_load) == 2,
          "unrestricted low-pressure live-in shape changed");
}

void ordinary_pressure_uses_both_unbound_low_slots() {
  std::array<std::uint32_t, 19> guest{};
  for (unsigned index = 0; index < 18U; ++index)
    guest[index] =
        modified_immediate(false, false, 0x40U + index, index);
  guest.back() = bitwise(4U, 16U, 16U, 17U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x53'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "unconstrained 18-vector pressure block did not compile");
  /*
   * The prior high-bank-only allocation emitted 58 hot / 60 total words for
   * this block. Keeping V16/V17 resident removes two imports and one duplicate
   * state write; tail alignment absorbs one of the three total-word savings.
   */
  require(compiled.hot_words == 55U && compiled.words.size() == 58U,
          "ordinary q6/q7 capacity lost its exact code-size reduction");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  for (unsigned index = 0; index < 18U; ++index) {
    const auto native =
        find_native_operation(hot, guest[index], 31U);
    require(native.has_value(),
            "ordinary pressure block omitted a modified immediate");
    const unsigned expected =
        index < 16U ? 16U + index : 6U + (index - 16U);
    require(destination(*native) == expected,
            "ordinary pressure allocation order changed");
  }

  const auto consumer =
      find_native_operation(hot, guest.back(), register_mask);
  require(consumer.has_value(),
          "ordinary low-slot consumer omitted its native word");
  require(left(*consumer) == 6U && right(*consumer) == 7U &&
              destination(*consumer) == 6U,
          "ordinary q6/q7 values were staged or spilled before reuse");
  require(std::none_of(hot.begin(), hot.end(),
                       is_cpu_state_vector_load),
          "ordinary q6/q7 reuse reloaded CpuState");
}

void one_constrained_pin_leaves_one_low_slot_for_ordinary_pressure() {
  constexpr std::array ordinary_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 13U, 14U, 16U,
  };
  std::array<std::uint32_t, 19> guest{};
  guest[0] = 0x4f40'8000U | (15U << 16U) |
             (15U << 5U) | 15U;
  for (std::size_t index = 0; index < ordinary_guests.size(); ++index)
    guest[index + 1U] =
        modified_immediate(false, false, 0x60U + index,
                           ordinary_guests[index]);
  guest[17] = modified_immediate(false, false, 0xa4U, 17U);
  guest[18] = bitwise(4U, 17U, 17U, 0U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x54'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "single-pin pressure block did not compile");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  const auto constrained =
      find_native_operation(hot, guest.front(),
                            constrained_register_mask);
  const auto low_definition =
      find_native_operation(hot, guest[17], 31U);
  const auto low_consumer =
      find_native_operation(hot, guest[18], register_mask);
  require(constrained.has_value() && low_definition.has_value() &&
              low_consumer.has_value(),
          "single-pin pressure block omitted a native operation");
  require(left(*constrained) == 6U && right(*constrained) == 6U &&
              destination(*constrained) == 6U,
          "preselected constrained identity lost q6");
  require(destination(*low_definition) == 7U &&
              left(*low_consumer) == 7U &&
              destination(*low_consumer) == 7U,
          "single constrained pin did not leave q7 for ordinary pressure");
}

void two_constrained_pins_leave_no_low_slot_for_ordinary_pressure() {
  constexpr std::array ordinary_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 13U, 16U, 17U,
  };
  std::array<std::uint32_t, 19> guest{};
  guest[0] = 0x4f40'8000U | (14U << 16U) |
             (14U << 5U) | 14U;
  guest[1] = 0x4f40'8000U | (15U << 16U) |
             (15U << 5U) | 15U;
  for (std::size_t index = 0; index < ordinary_guests.size(); ++index)
    guest[index + 2U] =
        modified_immediate(false, false, 0x80U + index,
                           ordinary_guests[index]);
  guest.back() = modified_immediate(false, false, 0xd2U, 18U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x55'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "two-pin pressure block did not compile");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::array<std::uint32_t, 2> pins{};
  std::size_t pin_count{};
  for (const auto word : hot) {
    if ((word & ~constrained_register_mask) !=
        (guest[0] & ~constrained_register_mask))
      continue;
    require(pin_count < pins.size(),
            "two-pin pressure block emitted an extra constrained word");
    pins[pin_count++] = word;
  }
  const auto overflow =
      find_native_operation(hot, guest.back(), 31U);
  require(pin_count == pins.size() && overflow.has_value(),
          "two-pin pressure block omitted a native operation");
  require(right(pins[0]) == 6U && right(pins[1]) == 7U,
          "preselected constrained identities changed low bindings");
  require(destination(*overflow) == 0U,
          "ordinary pressure consumed a constrained low slot");
}

void require_vector_live_ins(std::string_view name,
                            std::uint32_t instruction,
                            std::size_t expected) {
  const std::array guest{instruction};
  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  require(compiled.guest_instructions == 1U,
          "JIT did not compile a vector live-in test instruction");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated vector live-in code");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::size_t actual{};
  for (const auto word : hot)
    actual += is_cpu_state_vector_load(word);
  if (actual != expected) {
    std::fprintf(stderr,
                 "%.*s imported %zu guest vectors; expected %zu\n",
                 static_cast<int>(name.size()), name.data(), actual,
                 expected);
    std::abort();
  }
}

void modified_immediates_name_resident_destination_directly() {
  const auto check = [](std::string_view name, std::uint32_t instruction,
                        std::size_t expected_live_ins, std::uint64_t pc) {
    const std::array guest{instruction};
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a modified-immediate instruction");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~31U) != (instruction & ~31U))
        continue;
      require(!native.has_value(),
              "JIT emitted a modified-immediate instruction twice");
      native = word;
    }
    if (!native.has_value()) {
      std::fprintf(stderr, "%.*s was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(destination(*native) >= first_cached_vector &&
                destination(*native) <= last_cached_vector,
            "modified-immediate destination escaped the resident cache");
    require(live_ins == expected_live_ins,
            "modified-immediate imported the wrong destination state");
  };

  check("modified-immediate overwrite",
        modified_immediate(false, false, 0xa6U, 5U), 0U,
        code_base + 0x24'000U);
  check("modified-immediate OR combine",
        modified_immediate(true, false, 0x93U, 6U), 1U,
        code_base + 0x25'000U);
  check("modified-immediate clear combine",
        modified_immediate(true, true, 0x57U, 7U), 1U,
        code_base + 0x26'000U);
}

void modified_immediate_full_cache_fallback_is_exact() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t target,
                         std::size_t expected_live_ins, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache immediate sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~31U) == (target & ~31U))
        native = word;
    }
    if (!native.has_value()) {
      std::fprintf(stderr, "%.*s fallback was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(destination(*native) == 0U,
            "full-cache modified-immediate did not use fallback v0");
    require(live_ins == expected_live_ins,
            "full-cache modified-immediate imported the wrong state");
  };

  check("overwrite immediate",
        modified_immediate(false, false, 0x39U, 31U), 10U,
        code_base + 0x27'000U);
  check("combining immediate",
        modified_immediate(true, false, 0x39U, 31U), 11U,
        code_base + 0x28'000U);
}

void require_direct_two_register_operation(std::string_view name,
                                           std::uint32_t instruction,
                                           bool source_is_gpr,
                                           std::uint64_t pc) {
  const std::array guest{instruction};
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == 1U,
          "JIT did not compile a direct two-register SIMD operation");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> native;
  for (const auto word : hot) {
    if ((word & ~two_register_mask) !=
        (instruction & ~two_register_mask))
      continue;
    if (native.has_value()) {
      std::fprintf(stderr, "%.*s was emitted more than once\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    native = word;
  }
  if (!native.has_value()) {
    std::fprintf(stderr, "%.*s was not emitted directly\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  const auto native_destination = destination(*native);
  const auto native_source = left(*native);
  require(native_destination >= first_cached_vector &&
              native_destination <= last_cached_vector,
          "direct SIMD destination escaped the resident vector cache");
  if (source_is_gpr) {
    require(is_cached_gpr(native_source),
            "direct SIMD source escaped the resident GPR cache");
    return;
  }
  require(native_source >= first_cached_vector &&
              native_source <= last_cached_vector,
          "direct SIMD source escaped the resident vector cache");
  require((native_source == native_destination) ==
              (left(instruction) == destination(instruction)),
          "direct SIMD source/destination alias was not preserved");
}

void lane_reduce_and_shift_families_name_resident_registers_directly() {
  /*
   * Every case is a single native instruction. Alias cases additionally
   * prove that source allocation happens before destination definition.
   */
  require_direct_two_register_operation(
      "DUP (general)", 0x4e08'0c20U, true, code_base + 0xa000U);
  require_direct_two_register_operation(
      "INS (general)", 0x4e08'1c20U, true, code_base + 0xb000U);
  require_direct_two_register_operation(
      "INS (element)", 0x6e08'4420U, false, code_base + 0xc000U);
  require_direct_two_register_operation(
      "aliased INS (element)", 0x6e08'44a5U, false,
      code_base + 0xd000U);
  require_direct_two_register_operation(
      "aliased DUP (element)", 0x4e04'04c6U, false,
      code_base + 0xe000U);
  require_direct_two_register_operation(
      "aliased CNT", 0x4e20'5929U, false, code_base + 0xf000U);
  require_direct_two_register_operation(
      "aliased CMEQ (zero)", 0x4e20'9929U, false,
      code_base + 0x10'000U);
  require_direct_two_register_operation(
      "aliased ADDV", 0x4e31'b94aU, false, code_base + 0x11'000U);
  require_direct_two_register_operation(
      "aliased scalar ADDP", 0x5ef1'b96bU, false,
      code_base + 0x12'000U);
  require_direct_two_register_operation(
      "aliased SSRA", 0x4f38'158cU, false, code_base + 0x13'000U);
  require_direct_two_register_operation(
      "SHRN2", 0x4f18'842eU, false, code_base + 0x14'000U);
  require_direct_two_register_operation(
      "aliased SSHLL2", 0x4f0c'a5efU, false,
      code_base + 0x15'000U);
}

void narrowing_move_fallback_alias_is_preserved() {
  /*
   * Bind all eighteen resident vector slots, then use an unbound aliased
   * V31 source/destination. The fallback path must keep the native input and
   * output registers distinct; XTN2 additionally needs a second import for
   * the old lower destination half.
   */
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](bool upper, std::size_t expected_live_ins,
                         std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    const auto target = 0x0e21'2800U |
                        (upper ? 0x4000'0000U : 0U) |
                        (31U << 5U) | 31U;
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache narrowing sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

    std::optional<std::uint32_t> native_target;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~0x3ffU) == (target & ~0x3ffU))
        native_target = word;
    }
    require(native_target.has_value(),
            "JIT did not emit the fallback narrowing move");
    require(left(*native_target) != destination(*native_target),
            "fallback narrowing move collapsed aliased input and output");
    require(live_ins == expected_live_ins,
            "fallback narrowing move imported the wrong vector state");
  };

  check(false, 11U, code_base + 0x8000U);
  check(true, 12U, code_base + 0x9000U);
}

void lane_and_shift_fallback_aliases_are_preserved() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t target,
                         std::size_t expected_live_ins, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache lane/shift sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

    std::optional<std::uint32_t> native_target;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~two_register_mask) ==
          (target & ~two_register_mask))
        native_target = word;
    }
    if (!native_target.has_value()) {
      std::fprintf(stderr, "%.*s fallback was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(left(*native_target) == 1U &&
                destination(*native_target) == 0U,
            "full-cache SIMD alias did not use v1 input and v0 output");
    require(live_ins == expected_live_ins,
            "full-cache SIMD alias imported the wrong vector state");
  };

  check("aliased DUP (element)", 0x4e04'07ffU, 11U,
        code_base + 0x16'000U);
  check("aliased INS (element)", 0x6e08'47ffU, 12U,
        code_base + 0x17'000U);
  check("aliased SSRA", 0x4f38'17ffU, 12U,
        code_base + 0x18'000U);
}

void remaining_floating_fallback_aliases_are_preserved() {
  /*
   * Occupy all eighteen resident vector slots, then use V31 as every operand.
   * A zeroing destination must not overwrite the fallback source before the
   * native operation reads it.
   */
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t operation,
                         std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    const auto target =
        operation | (31U << 16U) | (31U << 5U) | 31U;
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a full-cache floating alias sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native_target;
    for (const auto word : hot)
      if ((word & ~register_mask) == (target & ~register_mask))
        native_target = word;
    if (!native_target.has_value()) {
      std::fprintf(stderr, "%.*s is missing its fallback native operation\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(left(*native_target) == right(*native_target),
            "aliased floating fallback sources became distinct values");
    require(destination(*native_target) != left(*native_target),
            "floating fallback destination clobbered its aliased source");
  };

  check("vector horizontal pairwise add", 0x6e20'd400U,
        code_base + 0x22'000U);
  check("scalar reciprocal step", 0x5e20'fc00U,
        code_base + 0x23'000U);
}

void remaining_simd_arithmetic_full_cache_fallbacks_are_exact() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto compile = [&](std::uint32_t target, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a full-cache SIMD sequence");
    return compiled;
  };
  const auto check_unrestricted =
      [&](std::string_view name, std::uint32_t operation,
          std::size_t expected_live_ins, std::uint64_t pc) {
        const auto target =
            operation | (31U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~register_mask) == (target & ~register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its full-cache native operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == 0U && right(*native) == 0U &&
                    destination(*native) == 2U,
                "full-cache unrestricted SIMD fallback used wrong registers");
        require(live_ins == expected_live_ins,
                "full-cache unrestricted SIMD fallback imported wrong state");
      };
  const auto check_restricted =
      [&](std::string_view name, std::uint32_t operation,
          std::uint64_t pc) {
        constexpr std::uint32_t restricted_register_mask =
            (15U << 16U) | (31U << 5U) | 31U;
        const auto target =
            operation | (15U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~restricted_register_mask) ==
              (target & ~restricted_register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its restricted fallback operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == 1U && right(*native) == 6U &&
                    destination(*native) == 0U,
                "full-cache pinned SIMD operation used wrong registers");
        require(live_ins == 12U,
                "full-cache pinned SIMD operation imported wrong state");
      };
  const auto check_table =
      [&](std::string_view name, std::uint32_t operation,
          std::size_t expected_live_ins, unsigned native_first,
          unsigned native_index, unsigned native_destination,
          std::uint64_t pc) {
        const auto target =
            operation | (31U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~register_mask) == (target & ~register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its full-cache table operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == native_first &&
                    right(*native) == native_index &&
                    destination(*native) == native_destination,
                "full-cache table fallback used wrong registers");
        require(live_ins == expected_live_ins,
                "full-cache table fallback imported wrong state");
      };

  const auto &host = ::uml::platform::apple::probed_host_capabilities();
  if (!host.advanced_simd)
    return;
  check_unrestricted(
      "widening accumulator", 0x0e20'5000U, 12U,
      code_base + 0x3e'000U);
  check_unrestricted(
      "vector permute", 0x4e00'2800U, 11U,
      code_base + 0x3f'000U);
  check_restricted(
      "halfword indexed integer accumulator", 0x6f40'0000U,
      code_base + 0x40'000U);
  check_table(
      "single-table TBL", 0x4e00'0000U, 12U, 0U, 4U, 5U,
      code_base + 0x41'000U);
  check_table(
      "single-table TBX", 0x4e00'1000U, 13U, 0U, 4U, 5U,
      code_base + 0x42'000U);
  check_table(
      "two-table TBL", 0x4e00'2000U, 11U, 30U, 30U, 30U,
      code_base + 0x43'000U);

  if (host.fp16 && host.floating_point)
    check_restricted(
        "half indexed floating accumulator", 0x4f00'1000U,
        code_base + 0x44'000U);
  if (host.has(::uml::cpu::ArmFeature::fhm))
    check_restricted(
        "half indexed fused-long accumulator", 0x4f80'0000U,
        code_base + 0x45'000U);
  if (host.has(::uml::cpu::ArmFeature::bf16))
    check_restricted(
        "bfloat indexed multiply-long accumulator", 0x4fc0'f000U,
        code_base + 0x46'000U);
}

void destination_live_ins_are_translation_time_exact() {
  /*
   * Rd, Rn and Rm are deliberately distinct. These checks count generated
   * CpuState imports, so a false reads_old decision is visible even though it
   * does not change architectural results.
   */
  constexpr unsigned destination = 0U;
  constexpr unsigned first = 1U;
  constexpr unsigned second = 2U;
  constexpr auto fields = (second << 16U) | (first << 5U) | destination;
  constexpr auto unary_fields = (first << 5U) | destination;

  require_vector_live_ins(
      "scalar FADD",
      0x1e20'0800U | (2U << 12U) | fields, 2U);
  require_vector_live_ins(
      "vector FSQRT", 0x6ea1'f800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector FRINTN", 0x4e21'8800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector FABS", 0x4ea0'f800U | unary_fields, 1U);

  require_vector_live_ins(
      "floating compare with register",
      0x1e20'2000U | fields, 2U);
  require_vector_live_ins(
      "floating compare with zero",
      0x1e20'2008U | unary_fields, 1U);

  /* FCVTL2 selects the upper source half but does not preserve Rd. */
  require_vector_live_ins(
      "upper widening conversion", 0x4e21'7800U | unary_fields, 1U);
  /* FCVTN2 writes the upper destination half and preserves the lower half. */
  require_vector_live_ins(
      "upper narrowing conversion", 0x4e21'6800U | unary_fields, 2U);
  /* An aliased conversion must import Rn before the allocator defines Rd. */
  require_vector_live_ins(
      "aliased scalar integer conversion", 0x5e79'd800U, 1U);

  /* XTN overwrites Rd; XTN2 alone preserves the lower destination half. */
  require_vector_live_ins(
      "lower narrowing move", 0x0e21'2800U | unary_fields, 1U);
  require_vector_live_ins(
      "upper narrowing move", 0x4e21'2800U | unary_fields, 2U);
  /* The lower zeroing form still needs its aliased source imported. */
  require_vector_live_ins(
      "aliased lower narrowing move",
      0x0e21'2800U | (first << 5U) | first, 1U);
  require_vector_live_ins(
      "aliased upper narrowing move",
      0x4e21'2800U | (first << 5U) | first, 1U);

  require_vector_live_ins(
      "vector reciprocal estimate", 0x4ea1'd800U | unary_fields, 1U);
  require_vector_live_ins(
      "scalar reciprocal step", 0x5e20'fc00U | fields, 2U);

  /*
   * These scalar forms merge into their old destination under FPCR.NEP, so
   * they import Rd exactly where the virtual CPU implements FEAT_AFP. The
   * profile is assumed, not probed: translation alone never runs it.
   */
  for (const bool afp : {false, true}) {
    auto host = ::uml::platform::apple::probed_host_capabilities();
    host.supported.reset(static_cast<std::size_t>(::uml::cpu::ArmFeature::afp));
    if (afp)
      for (const auto feature :
           {::uml::cpu::ArmFeature::afp, ::uml::cpu::ArmFeature::fp16,
            ::uml::cpu::ArmFeature::fhm, ::uml::cpu::ArmFeature::bf16,
            ::uml::cpu::ArmFeature::frintts, ::uml::cpu::ArmFeature::fcma,
            ::uml::cpu::ArmFeature::jscvt})
        host.supported.set(static_cast<std::size_t>(feature));
    require(::uml::cpu::arm64::implements_afp(host) == afp,
            "assumed FEAT_AFP profile does not select the twins");
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    const std::size_t scalar_destination_live_ins = afp ? 2U : 1U;
    require_vector_live_ins(
        "scalar FSQRT", 0x1e21'c000U | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar FRINTN", 0x1e24'4000U | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar FABS", 0x1e20'c000U | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar precision conversion",
        0x1e22'4000U | (1U << 15U) | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar reciprocal estimate", 0x5ea1'd800U | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar integer conversion", 0x5e21'd800U | unary_fields,
        scalar_destination_live_ins);
    require_vector_live_ins(
        "scalar conversion from a general register",
        0x1e22'0000U | unary_fields, scalar_destination_live_ins - 1U);
    /* Merging into a source needs no extra import: sources are whole. */
    require_vector_live_ins(
        "scalar FADD", 0x1e20'0800U | (2U << 12U) | fields, 2U);
    require_vector_live_ins(
        "scalar FMADD", 0x1f00'0000U | (3U << 10U) | fields, 3U);
  }

  require_vector_live_ins(
      "vector horizontal binary", 0x6e20'd400U | fields, 2U);
  require_vector_live_ins(
      "scalar horizontal pairwise", 0x7e70'd800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector horizontal reduction", 0x6e30'f800U | unary_fields, 1U);
}

void lane_reduce_and_shift_live_ins_are_exact() {
  require_vector_live_ins(
      "DUP (general)", 0x4e08'0c20U, 0U);
  require_vector_live_ins(
      "INS (general)", 0x4e08'1c20U, 1U);
  require_vector_live_ins(
      "INS (element)", 0x6e08'4420U, 2U);
  require_vector_live_ins(
      "aliased INS (element)", 0x6e08'44a5U, 1U);
  require_vector_live_ins(
      "aliased DUP (element)", 0x4e04'04c6U, 1U);
  require_vector_live_ins(
      "aliased CNT", 0x4e20'5929U, 1U);
  require_vector_live_ins(
      "aliased CMEQ (zero)", 0x4e20'9929U, 1U);
  require_vector_live_ins(
      "aliased ADDV", 0x4e31'b94aU, 1U);
  require_vector_live_ins(
      "aliased scalar ADDP", 0x5ef1'b96bU, 1U);
  require_vector_live_ins(
      "SSRA", 0x4f38'1420U, 2U);
  require_vector_live_ins(
      "aliased SSRA", 0x4f38'158cU, 1U);
  require_vector_live_ins(
      "SHL", 0x4f28'5420U, 1U);
  require_vector_live_ins(
      "SHRN2", 0x4f18'8420U, 2U);
  require_vector_live_ins(
      "aliased SHRN2", 0x4f18'84a5U, 1U);
  require_vector_live_ins(
      "aliased SSHLL2", 0x4f0c'a5efU, 1U);
}

void remaining_simd_arithmetic_live_ins_are_exact() {
  constexpr unsigned destination = 0U;
  constexpr unsigned left_source = 1U;
  constexpr unsigned right_source = 2U;
  constexpr auto fields =
      (right_source << 16U) | (left_source << 5U) | destination;
  const auto &host = ::uml::platform::apple::probed_host_capabilities();
  if (!host.advanced_simd)
    return;

  require_vector_live_ins(
      "widening SABAL accumulator", 0x0e20'5000U | fields, 3U);
  require_vector_live_ins(
      "widening SADDL overwrite", 0x0e20'0000U | fields, 2U);

  require_vector_live_ins(
      "word indexed MLA accumulator", 0x6f80'0000U | fields, 3U);
  require_vector_live_ins(
      "word indexed MUL overwrite", 0x4f80'8000U | fields, 2U);
  require_vector_live_ins(
      "halfword indexed MLA accumulator", 0x6f40'0000U | fields, 3U);
  require_vector_live_ins(
      "halfword indexed MUL overwrite", 0x4f40'8000U | fields, 2U);

  require_vector_live_ins(
      "TBL single-table overwrite", 0x4e00'0000U | fields, 2U);
  require_vector_live_ins(
      "TBX single-table accumulator", 0x4e00'1000U | fields, 3U);
  require_vector_live_ins(
      "TBL two-table overwrite",
      0x4e00'2000U | (3U << 16U) | (1U << 5U), 3U);
  require_vector_live_ins(
      "TRN1 overwrite", 0x4e00'2800U | fields, 2U);

  if (host.floating_point) {
    require_vector_live_ins(
        "word indexed FMLA accumulator", 0x4f80'1000U | fields, 3U);
    require_vector_live_ins(
        "word indexed FMUL overwrite", 0x4f80'9000U | fields, 2U);
  }
  if (host.fp16 && host.floating_point) {
    require_vector_live_ins(
        "half indexed FMLA accumulator", 0x4f00'1000U | fields, 3U);
    require_vector_live_ins(
        "half indexed FMUL overwrite", 0x4f00'9000U | fields, 2U);
  }

  if (host.has(::uml::cpu::ArmFeature::bf16)) {
    require_vector_live_ins(
        "bfloat vector dot accumulator", 0x6e40'fc00U | fields, 3U);
    require_vector_live_ins(
        "bfloat indexed dot accumulator", 0x4f40'f000U | fields, 3U);
    require_vector_live_ins(
        "bfloat vector multiply-long accumulator",
        0x6ec0'fc00U | fields, 3U);
    require_vector_live_ins(
        "bfloat indexed multiply-long accumulator",
        0x4fc0'f000U | fields, 3U);
    require_vector_live_ins(
        "bfloat matrix accumulator", 0x6e40'ec00U | fields, 3U);
  }
  if (host.has(::uml::cpu::ArmFeature::fhm)) {
    require_vector_live_ins(
        "half vector fused-long accumulator",
        0x4e20'ec00U | fields, 3U);
    require_vector_live_ins(
        "half indexed fused-long accumulator",
        0x4f80'0000U | fields, 3U);
  }
  if (host.has(::uml::cpu::ArmFeature::dotprod)) {
    require_vector_live_ins(
        "integer vector dot accumulator", 0x4e80'9400U | fields, 3U);
    require_vector_live_ins(
        "integer indexed dot accumulator", 0x4f80'e000U | fields, 3U);
  }
  if (host.has(::uml::cpu::ArmFeature::i8mm))
    require_vector_live_ins(
        "integer matrix accumulator", 0x4e80'a400U | fields, 3U);
}

} // namespace

void run_vector_remap_pressure_tests() {
  unrestricted_low_pressure_shape_is_unchanged();
  ordinary_pressure_uses_both_unbound_low_slots();
  one_constrained_pin_leaves_one_low_slot_for_ordinary_pressure();
  two_constrained_pins_leave_no_low_slot_for_ordinary_pressure();
  modified_immediates_name_resident_destination_directly();
  modified_immediate_full_cache_fallback_is_exact();
  narrowing_move_fallback_alias_is_preserved();
  lane_and_shift_fallback_aliases_are_preserved();
  remaining_floating_fallback_aliases_are_preserved();
  remaining_simd_arithmetic_full_cache_fallbacks_are_exact();
  destination_live_ins_are_translation_time_exact();
  lane_reduce_and_shift_families_name_resident_registers_directly();
  lane_reduce_and_shift_live_ins_are_exact();
  remaining_simd_arithmetic_live_ins_are_exact();
}
