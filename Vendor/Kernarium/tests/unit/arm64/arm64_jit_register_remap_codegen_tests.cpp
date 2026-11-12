// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t two_register_mask = (31U << 5U) | 31U;
constexpr std::uint32_t conditional_source_mask =
    (31U << 16U) | (31U << 5U);
constexpr unsigned darwin_platform_register = 18U;
constexpr unsigned first_cached_vector = 16U;
constexpr unsigned last_cached_vector = 31U;
constexpr std::uint64_t code_base = 0x4f00'0000ULL;

[[nodiscard]] constexpr bool is_cached_host_gpr(unsigned reg) noexcept {
  return (reg >= 3U && reg <= 7U) || reg == 12U || reg == 13U ||
         reg == 17U;
}

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
three_register(std::uint32_t operation, unsigned destination, unsigned left,
               unsigned right) noexcept {
  return operation | ((right & 31U) << 16U) | ((left & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
unsigned_memory(bool load, unsigned base, unsigned transfer) noexcept {
  return 0xf900'0000U | ((load ? 1U : 0U) << 22U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
conditional_compare(unsigned condition, unsigned left,
                    unsigned right) noexcept {
  return 0xfa40'0000U | ((right & 31U) << 16U) |
         ((condition & 15U) << 12U) | ((left & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
rotate_mask_flags(unsigned source, unsigned rotation,
                  unsigned mask) noexcept {
  return 0xba00'0400U | ((rotation & 63U) << 15U) |
         ((source & 31U) << 5U) | (mask & 15U);
}

[[nodiscard]] constexpr std::uint32_t
set_flags_from_register(bool sixteen, unsigned source) noexcept {
  return 0x3a00'080dU | (sixteen ? 0x4000U : 0U) |
         ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
checksum_word(unsigned destination, unsigned accumulator,
              unsigned source) noexcept {
  return 0x9ac0'4c00U | ((source & 31U) << 16U) |
         ((accumulator & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
tls_state_access(bool load, unsigned transfer) noexcept {
  constexpr auto offset = offsetof(uml::cpu::arm64::CpuState, tls);
  static_assert((offset % sizeof(std::uint64_t)) == 0U);
  static_assert((offset / sizeof(std::uint64_t)) < 4096U);
  return (load ? 0xf940'0000U : 0xf900'0000U) |
         ((offset / sizeof(std::uint64_t)) << 10U) |
         (15U << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr unsigned destination(std::uint32_t instruction) {
  return instruction & 31U;
}

[[nodiscard]] constexpr unsigned left(std::uint32_t instruction) {
  return (instruction >> 5U) & 31U;
}

[[nodiscard]] constexpr unsigned right(std::uint32_t instruction) {
  return (instruction >> 16U) & 31U;
}

struct RegisterMap final {
  std::array<std::optional<unsigned>, 32> host{};

  void bind(unsigned guest, unsigned native) {
    require(native != 31U,
            "ordinary guest register was rewritten to the zero register");
    require(native != darwin_platform_register,
            "generated operation used Darwin's reserved x18");
    auto &binding = host[guest];
    if (binding.has_value())
      require(*binding == native,
              "guest register changed host binding inside the block");
    else
      binding = native;
  }
};

void bind_instruction(RegisterMap &mapping, std::uint32_t guest,
                      std::uint32_t native) {
  if (left(guest) != right(guest))
    require(left(native) != right(native),
            "distinct live inputs share one host register");
  mapping.bind(destination(guest), destination(native));
  mapping.bind(left(guest), left(native));
  mapping.bind(right(guest), right(native));
}

[[nodiscard]] bool
is_direct_remapped_run(std::span<const std::uint32_t> native,
                       std::span<const std::uint32_t> guest) {
  if (native.size() < guest.size())
    return false;

  for (std::size_t index = 0; index < guest.size(); ++index)
    if ((native[index] & ~register_mask) !=
        (guest[index] & ~register_mask))
      return false;

  RegisterMap mapping;
  for (std::size_t index = 0; index < guest.size(); ++index)
    bind_instruction(mapping, guest[index], native[index]);

  /*
   * Each result is the left input of the following instruction. A matching
   * binding proves the value remains in its host register rather than being
   * written to and reloaded from CpuState between guest operations.
   */
  for (std::size_t index = 1; index < guest.size(); ++index)
    require(destination(native[index - 1]) == left(native[index]),
            "integer dependency was not kept resident");

  require(mapping.host[darwin_platform_register].has_value(),
          "test chain did not exercise guest x18");
  require(*mapping.host[darwin_platform_register] !=
              darwin_platform_register,
          "guest x18 was not remapped away from reserved host x18");
  return true;
}

void integer_dependency_chain_is_direct_and_resident() {
  /*
   * Keep every operation in a three-register form so the test can compare
   * opcode/immediate bits independently of the allocator's host-register
   * choices. Guest x18 deliberately exercises Darwin's platform register.
   */
  constexpr std::array guest{
      three_register(0x8b00'0000U, 18, 14, 13), // add x18, x14, x13
      three_register(0xca00'0000U, 15, 18, 14), // eor x15, x18, x14
      three_register(0x9ac0'2c00U, 14, 15, 13), // rorv x14, x15, x13
      three_register(0xcb00'0000U, 18, 14, 15), // sub x18, x14, x15
      three_register(0xaa00'0000U, 15, 18, 13), // orr x15, x18, x13
  };

  const auto compiled = jit_test::compile_block(code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete dependency chain");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated code");
  require(compiled.hot_words >= guest.size(),
          "JIT hot path is shorter than the guest operation run");

  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found, "JIT emitted the dependency chain more than once");
    found = true;
  }
  require(found,
          "JIT did not emit one consecutive native word per guest operation");
}

void checksum_dependency_chain_is_direct_and_resident() {
  if (!::uml::platform::apple::probed_host_capabilities().crc32)
    return;
  constexpr std::array guest{
      three_register(0x8b00'0000U, 18, 1, 2),
      checksum_word(6, 18, 2),
      three_register(0x8b00'0000U, 7, 6, 1),
  };

  const auto compiled = jit_test::compile_block(code_base + 0x9000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the checksum dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found, "JIT emitted the checksum chain more than once");
    found = true;
  }
  require(found, "checksum operation staged through fixed GPRs");
}

void thread_pointer_transfers_keep_guest_gprs_resident() {
  constexpr std::array guest{
      three_register(0x8b00'0000U, 5, 1, 2),
      0xd51b'd045U, // MSR TPIDR_EL0, X5
      0xd53b'd046U, // MRS X6, TPIDR_EL0
      three_register(0x8b00'0000U, 7, 6, 3),
  };
  const auto compiled =
      jit_test::compile_block(code_base + 0xa000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the thread-pointer dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 2> additions{};
  std::size_t addition_count{};
  std::optional<std::uint32_t> tls_store;
  std::optional<std::uint32_t> tls_load;
  for (const auto word : hot) {
    if ((word & ~register_mask) == 0x8b00'0000U &&
        is_cached_host_gpr(destination(word))) {
      require(addition_count < additions.size(),
              "JIT emitted an unexpected ADD in the thread-pointer chain");
      additions[addition_count++] = word;
    }
    if ((word & ~31U) == (tls_state_access(false, 0U) & ~31U))
      tls_store = word;
    if ((word & ~31U) == (tls_state_access(true, 0U) & ~31U))
      tls_load = word;
  }
  require(addition_count == additions.size(),
          "JIT did not emit both thread-pointer dependency operations");
  require(tls_store.has_value() && tls_load.has_value(),
          "JIT did not emit direct virtual thread-pointer transfers");
  require(destination(*tls_store) == destination(additions[0]),
          "thread-pointer write staged its resident source through x8");
  require(is_cached_host_gpr(destination(*tls_load)),
          "thread-pointer read did not target a resident host GPR");
  require(left(additions[1]) == destination(*tls_load),
          "thread-pointer read was stored and reloaded before its consumer");
}

void require_ordinary_memory_chain(
    std::span<const std::uint32_t> guest, std::uint64_t pc) {
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete ordinary-memory chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 2> additions{};
  std::array<std::uint32_t, 2> stores{};
  std::optional<std::uint32_t> load;
  std::size_t addition_count = 0;
  std::size_t store_count = 0;
  for (const auto word : hot) {
    if ((word & ~register_mask) == 0x8b00'0000U &&
        is_cached_host_gpr(destination(word)) &&
        is_cached_host_gpr(left(word)) &&
        is_cached_host_gpr(right(word))) {
      require(addition_count < additions.size(),
              "JIT emitted an unexpected cached ADD in the memory chain");
      additions[addition_count++] = word;
    }
    if ((word & ~31U) == 0xf900'0100U) {
      require(store_count < stores.size(),
              "JIT emitted an unexpected direct ordinary store");
      stores[store_count++] = word;
    }
    if ((word & ~31U) == 0xf940'0100U) {
      require(!load.has_value(),
              "JIT emitted an unexpected direct ordinary load");
      load = word;
    }
  }

  require(addition_count == additions.size(),
          "JIT did not retain both arithmetic operations in cached GPRs");
  require(store_count == stores.size(),
          "JIT did not emit both direct ordinary stores");
  require(load.has_value(), "JIT did not emit the direct ordinary load");

  const auto loaded_host = destination(*load);
  require(destination(stores[0]) == destination(additions[0]),
          "resident arithmetic result was staged before its store");
  require(loaded_host == destination(stores[1]),
          "loaded resident value was staged before its following store");
  require(loaded_host == left(additions[1]),
          "loaded resident value was staged before its dependent use");
  require(right(additions[0]) == right(additions[1]),
          "live input changed host binding across memory operations");
  require(loaded_host != 9U && destination(stores[0]) != 9U,
          "ordinary memory data operand still uses the x9 scratch register");
}

void ordinary_memory_names_resident_data_registers_directly() {
  /*
   * Each first store consumes an arithmetic result. Each load result is then
   * reused by both another store and a dependent arithmetic operation. None
   * of those memory data operands needs an x9 staging move.
   */
  constexpr std::array unsigned_guest{
      three_register(0x8b00'0000U, 5, 1, 2), // add x5, x1, x2
      unsigned_memory(false, 10, 5),          // str x5, [x10]
      unsigned_memory(true, 10, 6),           // ldr x6, [x10]
      unsigned_memory(false, 10, 6),          // str x6, [x10]
      three_register(0x8b00'0000U, 7, 6, 2), // add x7, x6, x2
  };
  require_ordinary_memory_chain(unsigned_guest, code_base + 0x1000);

  constexpr std::array unscaled_guest{
      three_register(0x8b00'0000U, 5, 1, 2), // add x5, x1, x2
      0xf800'0145U,                           // stur x5, [x10]
      0xf840'0146U,                           // ldur x6, [x10]
      0xf800'0146U,                           // stur x6, [x10]
      three_register(0x8b00'0000U, 7, 6, 2), // add x7, x6, x2
  };
  require_ordinary_memory_chain(unscaled_guest, code_base + 0x2000);
}

void tlb_page_tag_uses_one_page_number_shift() {
  constexpr std::array guest{
      unsigned_memory(true, 0, 1), // ldr x1, [x0]
  };
  const auto compiled = jit_test::compile_block(code_base + 0xa000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the TLB page-tag shape test");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  /*
   * `uml::memory::TlbEntry` keys its ways by the guest page number, so the
   * generated tag must be one right shift of the address register. A tag in
   * any other form can never match a filled way, and every translated access
   * would silently take the preserving miss bridge.
   */
  constexpr std::uint32_t page_number_shift =
      0xd340'0000U | ((DARWIN_UML_GUEST_PAGE_SHIFT & 63U) << 16U) |
      (63U << 10U) | (8U << 5U) | 1U; // lsr x1, x8, #page shift
  require(std::ranges::count(hot, page_number_shift) == 1,
          "JIT did not form the TLB page tag with one page-number shift");
}

void literal_load_names_its_resident_destination_directly() {
  constexpr std::array guest{
      0x5800'0005U,                           // ldr x5, literal
      three_register(0x8b00'0000U, 6, 5, 2), // add x6, x5, x2
  };
  const auto compiled = jit_test::compile_block(code_base + 0x3000, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the literal-load dependency");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> load;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~31U) == 0xf940'0100U)
      load = word;
    if ((word & ~register_mask) == 0x8b00'0000U &&
        is_cached_host_gpr(destination(word)) &&
        is_cached_host_gpr(left(word)) &&
        is_cached_host_gpr(right(word)))
      consumer = word;
  }
  require(load.has_value(), "JIT did not emit the native literal load");
  require(consumer.has_value(),
          "JIT did not emit the literal load's cached consumer");
  require(destination(*load) == left(*consumer),
          "literal load destination was staged before its dependent use");
  require(destination(*load) != 9U,
          "literal load destination still uses the x9 scratch register");
}

void cross_domain_chain_is_direct_and_resident() {
  /*
   * Guest register numbers deliberately overlap across the GPR and vector
   * namespaces. Each result feeds the next instruction in the other namespace.
   * Where the virtual CPU implements FEAT_AFP, a scalar SCVTF keeps the rest
   * of its old destination under FPCR.NEP, so the JIT loads d6 from CpuState
   * into its resident register just before the conversion.
   */
  constexpr std::array guest{
      std::uint32_t{0xd280'00a5U}, // movz x5, #5
      std::uint32_t{0x9e67'00a5U}, // fmov d5, x5
      std::uint32_t{0x9e66'00a6U}, // fmov x6, d5
      std::uint32_t{0x9e62'00c6U}, // scvtf d6, x6
      std::uint32_t{0x9e78'00c7U}, // fcvtzs x7, d6
  };
  constexpr std::size_t conversion = 3U;
  constexpr std::uint32_t old_d6_load =
      0x3dc0'0000U | (15U << 5U) |
      static_cast<std::uint32_t>(
          (offsetof(uml::cpu::arm64::CpuState, vectors) +
           6U * sizeof(uml::cpu::arm64::VectorRegister)) /
          16U)
          << 10U; // ldr q<d6>, [x15, #vectors[6]]
  const bool merges =
      uml::cpu::arm64::implements_afp(uml::cpu::host_capabilities());
  const std::size_t length = guest.size() + (merges ? 1U : 0U);
  const auto compiled = jit_test::compile_block(code_base + 0x4000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete cross-domain chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  bool found = false;
  for (std::size_t start = 0; start + length <= hot.size(); ++start) {
    const auto chain = hot.subspan(start, length);
    std::array<std::uint32_t, guest.size()> native{};
    for (std::size_t index = 0; index < guest.size(); ++index)
      native[index] = chain[merges && index >= conversion ? index + 1U : index];
    bool opcodes_match =
        !merges || (chain[conversion] & ~31U) == old_d6_load;
    for (std::size_t index = 0; index < guest.size(); ++index) {
      const auto mask = index == 0U ? 31U : two_register_mask;
      opcodes_match &= (native[index] & ~mask) == (guest[index] & ~mask);
    }
    if (!opcodes_match)
      continue;

    const auto x5 = destination(native[0]);
    const auto v5 = destination(native[1]);
    const auto x6 = destination(native[2]);
    const auto v6 = destination(native[3]);
    const auto x7 = destination(native[4]);
    require(left(native[1]) == x5,
            "GPR value was staged before general-to-vector move");
    require(left(native[2]) == v5,
            "vector value was staged before vector-to-general move");
    require(left(native[3]) == x6,
            "GPR value was staged before integer-to-floating conversion");
    require(left(native[4]) == v6,
            "vector value was staged before floating-to-integer conversion");
    for (const auto host : {x5, x6, x7})
      require(is_cached_host_gpr(host),
              "cross-domain result escaped the GPR cache");
    for (const auto host : {v5, v6})
      require(host >= first_cached_vector && host <= last_cached_vector,
              "cross-domain result escaped the vector cache");
    require(!merges || destination(chain[conversion]) == v6,
            "SCVTF did not load its old destination for the NEP merge");
    require(!found, "JIT emitted the cross-domain chain more than once");
    found = true;
  }
  require(found,
          "JIT did not emit one native word per cross-domain operation");
}

void javascript_conversion_writes_a_resident_gpr_directly() {
  if (!::uml::platform::apple::probed_host_capabilities().has(
          ::uml::cpu::ArmFeature::jscvt))
    return;
  constexpr std::array guest{
      std::uint32_t{0x1e7e'0128U}, // fjcvtzs w8, d9
  };
  const auto compiled = jit_test::compile_block(code_base + 0x5000U, guest);
  require(compiled.guest_instructions == 1U,
          "JIT did not compile the JavaScript conversion");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::size_t> conversion;
  std::optional<std::size_t> source_copy;
  for (std::size_t index = 0; index < hot.size(); ++index) {
    if ((hot[index] & ~two_register_mask) == 0x1e7e'0000U)
      conversion = index;
    if ((hot[index] & ~(31U << 5U)) == 0x9e66'000aU)
      source_copy = index;
  }
  require(conversion.has_value(),
          "JIT did not emit the native JavaScript conversion");
  const auto &host = ::uml::platform::apple::probed_host_capabilities();
  require(source_copy.has_value() ==
              !host.jscvt_subnormal_flags_correct,
          "JIT selected the wrong host-status correction shape");
  if (source_copy)
    require(*source_copy == *conversion + 1U,
            "JIT staged the conversion result before indicator correction");
  const auto native_source = left(hot[*conversion]);
  const auto native_destination = destination(hot[*conversion]);
  require(native_source >= first_cached_vector &&
              native_source <= last_cached_vector,
          "JavaScript conversion source escaped the vector cache");
  require(is_cached_host_gpr(native_destination),
          "JavaScript conversion escaped its resident GPR destination");
  if (source_copy)
    require(left(hot[*source_copy]) == native_source,
            "indicator correction did not reuse the resident vector source");
}

void require_conditional_compare_sources(unsigned condition,
                                         std::uint64_t pc) {
  const std::array guest{
      three_register(0x8b00'0000U, 5, 1, 2), // add x5, x1, x2
      three_register(0x8b00'0000U, 6, 3, 4), // add x6, x3, x4
      conditional_compare(condition, 5, 6),
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the conditional-compare chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 2> producers{};
  std::size_t producer_count = 0;
  std::optional<std::uint32_t> compare;
  const auto expected_compare =
      condition < 14U ? guest[2] & ~conditional_source_mask
                      : std::uint32_t{0xeb00'001fU};
  for (const auto word : hot) {
    if ((word & ~register_mask) == 0x8b00'0000U &&
        is_cached_host_gpr(destination(word)) &&
        is_cached_host_gpr(left(word)) &&
        is_cached_host_gpr(right(word))) {
      require(producer_count < producers.size(),
              "JIT emitted an unexpected cached ADD before CCMP");
      producers[producer_count++] = word;
    }
    if ((word & ~conditional_source_mask) == expected_compare)
      compare = word;
  }

  require(producer_count == producers.size(),
          "JIT did not emit both conditional-compare producers");
  require(compare.has_value(),
          "JIT did not emit the native conditional compare");
  require(left(*compare) == destination(producers[0]),
          "conditional-compare left input was staged through x8");
  require(right(*compare) == destination(producers[1]),
          "conditional-compare right input was staged through x9");
}

void conditional_compare_names_resident_sources_directly() {
  /* Exercise both the real conditional form and AL's translated SUBS form. */
  require_conditional_compare_sources(0U, code_base + 0x6000U);
  require_conditional_compare_sources(14U, code_base + 0x7000U);
}

void flag_manipulation_names_resident_source_directly() {
  if (!::uml::platform::apple::probed_host_capabilities().has(
          ::uml::cpu::ArmFeature::flag_m))
    return;
  constexpr std::array guest{
      three_register(0x8b00'0000U, 5, 1, 2), // add x5, x1, x2
      rotate_mask_flags(5, 37, 0xb),
      set_flags_from_register(true, 5),
  };
  const auto compiled = jit_test::compile_block(code_base + 0x8000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the register-source flag chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> producer;
  std::optional<std::uint32_t> rotate_mask;
  std::optional<std::uint32_t> set_flags;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask) &&
        is_cached_host_gpr(destination(word)))
      producer = word;
    if ((word & ~(31U << 5U)) == (guest[1] & ~(31U << 5U)))
      rotate_mask = word;
    if ((word & ~(31U << 5U)) == (guest[2] & ~(31U << 5U)))
      set_flags = word;
  }
  require(producer.has_value(),
          "JIT did not emit the cached flag-source producer");
  require(rotate_mask.has_value(), "JIT did not emit native RMIF");
  require(set_flags.has_value(), "JIT did not emit native SETF16");
  require(left(*rotate_mask) == destination(*producer),
          "RMIF source was staged through x8");
  require(left(*set_flags) == destination(*producer),
          "SETF16 source was staged through x8");
}

} // namespace

int main() {
  integer_dependency_chain_is_direct_and_resident();
  checksum_dependency_chain_is_direct_and_resident();
  thread_pointer_transfers_keep_guest_gprs_resident();
  ordinary_memory_names_resident_data_registers_directly();
  tlb_page_tag_uses_one_page_number_shift();
  literal_load_names_its_resident_destination_directly();
  cross_domain_chain_is_direct_and_resident();
  javascript_conversion_writes_a_resident_gpr_directly();
  conditional_compare_names_resident_sources_directly();
  flag_manipulation_names_resident_source_directly();
  return 0;
}
