// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Streaming JIT translations, checked as code on injected SME profiles: no
 * AdvSIMD or other FP&SIMD word reaches an SM=1 translation, every SVE and
 * SME word in one is a form the profile implements in its mode, the host
 * enters streaming mode only in the external prologue and leaves it before
 * every host call and return, and only SM=1 translations touch the host's
 * streaming state: no other translation holds an SVE or SME word. No SM=1
 * translation takes the native FP environment or puts a guest FPSR in the
 * host's, since XNU resets FPSR on any kernel entry in streaming mode: it
 * only clears FPSR before a word that can raise an exception and reads it,
 * checking QC, right after. Every witness word of the A64 evidence table,
 * which the registration passes as the only argument, and every row of the
 * shared encoding table, is compiled in every mode and link shape.
 */
#include "arm64_sm0_vector_length_support.hpp"
#include "runtime_test_route.hpp"
#include "require.hpp"
#include "cpu/arm64/jit/jit_emitter.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/jit/jit_sme_transition.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/detail/sm0_vector_length.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using uml::testing::require;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_x0_one = 0x9100'0400U;
constexpr std::uint32_t blr_x16 = 0xd63f'0200U;
constexpr std::uint32_t ret = 0xd65f'03c0U;
constexpr std::uint32_t reload_cpu = 0xf940'0befU; // ldr x15, [sp, #16]

struct Witness final {
  std::string id;
  std::string section;
  std::string form;
  std::uint32_t word{};
};

[[nodiscard]] std::vector<Witness> witnesses(const char *bindings) {
  std::ifstream table{bindings};
  require(static_cast<bool>(table));
  std::vector<Witness> result;
  std::string line;
  std::getline(table, line); // header
  while (std::getline(table, line)) {
    std::istringstream fields{line};
    std::array<std::string, 6> column;
    for (auto &field : column)
      require(static_cast<bool>(std::getline(fields, field, '\t')));
    result.push_back({column[0], column[1], column[2],
                      static_cast<std::uint32_t>(
                          std::stoul(column[5], nullptr, 16))});
  }
  require(result.size() > 2000U);
  return result;
}

/* The host's own profile with the SME2 (or SME1-only) profile at `svl`,
 * or without SME at svl 0, and without FEAT_CSSC, so CNT takes its
 * fallback as on the M4. It carries no SVE token, as no virtual CPU does. */
[[nodiscard]] uml::cpu::HostCapabilities sme_host(unsigned svl,
                                                  bool sme2 = true) {
  auto host = ::uml::platform::without_sve(uml::cpu::host_capabilities());
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme2,
        uml::cpu::ArmFeature::sme_f64f64, uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature), svl != 0U);
  if (!sme2)
    host.supported.reset(static_cast<std::size_t>(uml::cpu::ArmFeature::sme2));
  host.supported.reset(static_cast<std::size_t>(uml::cpu::ArmFeature::cssc));
  host.cssc = false;
  host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
  return host;
}

/* An FP&SIMD data-processing word, or a load or store of SIMD&FP
 * registers. */
[[nodiscard]] constexpr bool fp_simd_word(std::uint32_t word) noexcept {
  const auto op0 = (word >> 25U) & 15U;
  return op0 == 7U || op0 == 15U || (word & 0x0e00'0000U) == 0x0c00'0000U;
}

constexpr std::uint32_t clear_fpsr = 0xd51b'443fU;    // msr fpsr, xzr
constexpr std::uint32_t read_fpsr_x11 = 0xd53b'442bU; // mrs x11, fpsr
constexpr std::uint32_t write_fpcr_x8 = 0xd51b'4408U; // msr fpcr, x8

/* TBZ W11, #27 (FPSR.QC), to any label. */
[[nodiscard]] constexpr bool qc_clear_branch(std::uint32_t word) noexcept {
  return (word & 0xfff8'001fU) == 0x36d8'000bU;
}

/* An FP&SIMD word that runs in streaming mode, as guest words the
 * profile marks FP-legal do. */
[[nodiscard]] constexpr bool streaming_fp(std::uint32_t word) noexcept {
  return arm64::detail::classify_fp_simd_streaming(word) ==
         arm64::detail::FpSimdStreaming::fp;
}

/* MRS or MSR of FPCR or FPSR. */
[[nodiscard]] constexpr bool fp_control_access(std::uint32_t word) noexcept {
  switch (word & 0xffff'ffe0U) {
  case 0xd53b'4400U:
  case 0xd51b'4400U:
  case 0xd53b'4420U:
  case 0xd51b'4420U:
    return true;
  default:
    return false;
  }
}

enum class Callee {
  enter,
  leave,
  streaming_bridge,
  preserving_bridge,
  helper,
};

[[nodiscard]] std::uintptr_t address(const void *function) noexcept {
  return arm64::callable_address(function);
}

/* The callee of the BLR X16 at `index`, whose LDR X16 (literal) is the
 * word before it. */
[[nodiscard]] Callee callee(const jit_test::CompiledCode &code,
                            std::size_t index) {
  require(index != 0U);
  const auto load = code.words[index - 1U];
  require((load & 0xff00'001fU) == 0x5800'0010U);
  const auto offset = static_cast<std::int64_t>(
      static_cast<std::int32_t>(((load >> 5U) & 0x7ffffU) << 13U) >> 13U);
  const auto cell =
      static_cast<std::size_t>(static_cast<std::int64_t>(index - 1U) + offset);
  require(cell >= code.executable_words && cell + 1U < code.words.size());
  const auto target = code.words[cell] |
                      (static_cast<std::uint64_t>(code.words[cell + 1U])
                       << 32U);
  if (target ==
      address(reinterpret_cast<const void *>(&uml_arm64_jit_sme_enter)))
    return Callee::enter;
  if (target ==
      address(reinterpret_cast<const void *>(&uml_arm64_jit_sme_leave)))
    return Callee::leave;
  if (target == address(reinterpret_cast<const void *>(
                    &uml_arm64_jit_tlb_translate_streaming)))
    return Callee::streaming_bridge;
  if (target == address(reinterpret_cast<const void *>(
                    &arm64::uml_arm64_jit_tlb_translate_preserving)))
    return Callee::preserving_bridge;
  return Callee::helper;
}

struct Checked final {
  std::size_t translations{};
  std::size_t helper_calls{};
};

/* Whether an SM=1 translation may hold this FPCR or FPSR access at
 * `index`: MSR FPCR from x8, which writes the guest's FPCR to the host's
 * too, and the FPSR capture around one word that can raise an exception:
 * MSR FPSR, XZR; the word; MRS X11, FPSR; TBZ W11, #27. */
[[nodiscard]] bool streaming_fp_control(const jit_test::CompiledCode &code,
                                        std::size_t index) {
  const auto word = code.words[index];
  if (word == write_fpcr_x8)
    return true;
  if (word == clear_fpsr)
    return index + 3U < code.executable_words &&
           code.words[index + 2U] == read_fpsr_x11;
  if (word == read_fpsr_x11)
    return index >= 2U && code.words[index - 2U] == clear_fpsr &&
           index + 1U < code.executable_words &&
           qc_clear_branch(code.words[index + 1U]);
  return false;
}

/* Whether an SVE or SME word can run on the host in this mode: the profile
 * implements its row there. */
[[nodiscard]] bool streaming_word_runs(std::uint32_t word, std::uint8_t mode) {
  const auto &host = uml::cpu::host_capabilities();
  const auto *row = arm64::detail::decode_sme(
      word, arm64::implemented_sme_features(host), host.sme_svl_bytes);
  return row != nullptr &&
         arm64::detail::sme_mode_permits(*row, (mode & arm64::svcr_sm) != 0U,
                                         (mode & arm64::svcr_za) != 0U);
}

void check_translation(const jit_test::CompiledCode &code, std::uint8_t mode,
                       const std::string &label, Checked &checked) {
  if (code.words.empty())
    return;
  require(code.executable_words != 0U &&
          code.executable_words <= code.words.size());
  const bool streaming = (mode & arm64::svcr_sm) != 0U;
  if (streaming && (code.native_fp || !code.accesses_fp_state)) {
    std::fprintf(stderr, "%s mode %u: native FP %d, FP state access %d\n",
                 label.c_str(), mode, code.native_fp, code.accesses_fp_state);
    require(false);
  }
  std::size_t enters = 0;
  /* Translated code after a helper would run outside streaming mode with
   * the guest's state spilled, so every helper must be terminal: the next
   * call after it is the epilogue's leave. */
  bool after_helper = false;
  for (std::size_t index = 0; index < code.executable_words; ++index) {
    const auto word = code.words[index];
    if (!streaming && arm64::detail::sme_encoding_space(word)) {
      std::fprintf(stderr, "%s mode %u: SVE or SME word %08x at %zu\n",
                   label.c_str(), mode, word, index);
      require(false);
    }
    if (streaming && ((fp_simd_word(word) && !streaming_fp(word)) ||
                      (fp_control_access(word) &&
                       !streaming_fp_control(code, index)))) {
      std::fprintf(stderr, "%s mode %u: FP&SIMD, FPCR or FPSR word %08x at %zu\n",
                   label.c_str(), mode, word, index);
      require(false);
    }
    if (streaming && arm64::detail::sme_encoding_space(word)) {
      if (!streaming_word_runs(word, mode)) {
        std::fprintf(stderr, "%s mode %u: SVE or SME word %08x at %zu does "
                     "not run in this mode\n", label.c_str(), mode, word,
                     index);
        require(false);
      }
      /* A MOVPRFX is always completed by its prefixed word. */
      const auto form = arm64::detail::find_sme_encoding(word)->form;
      if ((form == arm64::jit::sme_forms::movprfx ||
           form == arm64::jit::sme_forms::movprfx_predicated) &&
          (index + 1U >= code.executable_words ||
           !arm64::detail::sme_encoding_space(code.words[index + 1U]))) {
        std::fprintf(stderr, "%s mode %u: MOVPRFX %08x at %zu ends\n",
                     label.c_str(), mode, word, index);
        require(false);
      }
    }
    if (word == ret && streaming) {
      /* Every epilogue leaves streaming mode before restoring the frame,
       * with the vCPU pointer reloaded, since a helper may have changed
       * x15. */
      require(index >= 7U && code.words[index - 5U] == blr_x16);
      require(callee(code, index - 5U) == Callee::leave);
      require(code.words[index - 7U] == reload_cpu);
    }
    if (word != blr_x16)
      continue;
    const auto target = callee(code, index);
    if (streaming && after_helper) {
      require(target == Callee::leave);
      after_helper = false;
    }
    if (!streaming) {
      require(target == Callee::helper ||
              target == Callee::preserving_bridge);
      continue;
    }
    switch (target) {
    case Callee::enter:
      ++enters;
      require(index < code.internal_entry_word);
      break;
    case Callee::leave:
    case Callee::streaming_bridge:
      break;
    case Callee::preserving_bridge:
      require(false);
      break;
    case Callee::helper:
      /* A host helper call follows its own leave sequence. */
      require(index >= 3U && code.words[index - 2U] == blr_x16 &&
              callee(code, index - 2U) == Callee::leave);
      ++checked.helper_calls;
      after_helper = true;
      break;
    }
  }
  require(enters == (streaming ? 1U : 0U));
  require(!after_helper);
  ++checked.translations;
}

void check_program(std::uint64_t pc, std::span<const std::uint32_t> program,
                   const std::string &label, Checked &checked) {
  for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode)
    for (const auto shape :
         {jit_test::LinkShape::unlinked, jit_test::LinkShape::direct,
          jit_test::LinkShape::indirect, jit_test::LinkShape::tracked})
      check_translation(
          jit_test::compile_block_in_mode(pc, program, mode, shape).code,
          mode, label, checked);
}

void streaming_translations_hold_no_fp_simd_word(const char *bindings) {
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint64_t pc = 0x4f00'0000U;
  Checked checked;
  for (const auto &witness : witnesses(bindings)) {
    const std::array alone{witness.word, system_call};
    const std::array after{add_x0_one, witness.word, system_call};
    check_program(pc, alone, witness.id, checked);
    check_program(pc, after, witness.id, checked);
  }
  /* Sequences the compiler lowers as one unit: FEAT_MOPS prologue, main
   * and epilogue triples, exclusive pairs, unsigned-offset runs and CNT. */
  const std::vector<std::vector<std::uint32_t>> sequences{
      {0x1901'0440U, 0x1941'0440U, 0x1981'0440U, system_call}, // cpyfp/m/e
      {0x1d01'0440U, 0x1d41'0440U, 0x1d81'0440U, system_call}, // cpyp/m/e
      {0x19c2'0420U, 0x19c2'4420U, 0x19c2'8420U, system_call}, // setp/m/e
      {0xc85f'7c20U, 0xc802'7c20U, system_call},               // ldxr/stxr
      {0xf940'0020U, 0xf940'0421U, 0xf940'0822U, system_call}, // ldr run
      {0xf900'03e0U, 0xf900'07e1U, system_call},               // str sp run
      {0x3dc0'0020U, 0x3dc0'0421U, system_call},               // ldr q run
      {0xdac0'1c20U, 0x5ac0'1c20U, system_call},               // cnt x, w
      {0xd51b'4421U, 0xd53b'4422U, system_call},               // fpsr
      {0xd51b'4401U, 0xd53b'4402U, system_call},               // fpcr
  };
  for (const auto &sequence : sequences)
    check_program(pc, sequence, "sequence", checked);
  std::fprintf(stderr, "checked %zu translations, %zu helper calls\n",
               checked.translations, checked.helper_calls);
  require(checked.translations > 70'000U);
  require(checked.helper_calls != 0U);
}

/* Every translation, in every mode, is the same at every SVL: whatever
 * depends on it is read at run time. */
void translations_do_not_depend_on_the_svl(const char *bindings) {
  constexpr std::uint64_t pc = 0x4f20'0000U;
  const auto all = witnesses(bindings);
  std::vector<std::vector<std::uint32_t>> reference;
  {
    const auto host = sme_host(64);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (const auto &witness : all)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const std::array program{witness.word, system_call};
        reference.push_back(
            jit_test::compile_block_in_mode(pc, program, mode).code.words);
      }
  }
  for (const unsigned svl : {16U, 32U, 128U, 256U}) {
    const auto host = sme_host(svl);
    const uml::cpu::HostCapabilitiesScope scope{host};
    std::size_t index = 0;
    for (const auto &witness : all)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const std::array program{witness.word, system_call};
        const auto words =
            jit_test::compile_block_in_mode(pc, program, mode).code.words;
        if (words != reference[index]) {
          std::fprintf(stderr, "%s mode %u differs at SVL %u\n",
                       witness.id.c_str(), mode, svl);
          require(false);
        }
        ++index;
      }
  }
}

[[nodiscard]] bool illegal_in(std::uint32_t word, std::uint8_t mode) {
  const std::array program{word};
  return jit_test::compile_block_in_mode(0x4f30'0000U, program, mode)
      .code.guest_illegal;
}

/* The control core translates on the SME profile, each form only in the
 * modes its checks allow, and nothing of it without the profile. */
void control_core_follows_profile_and_mode() {
  struct Form final {
    std::uint32_t word;
    bool needs_za;
    bool needs_sme2;
  };
  constexpr std::array control{
      Form{0x04bf'5823U, false, false}, // rdsvl x3, #1
      Form{0x0424'5fe3U, false, false}, // addsvl x3, x4, #-1
      Form{0x047f'585fU, false, false}, // addspl sp, sp, #2
      Form{0xd503'477fU, false, false}, // smstart
      Form{0xd503'467fU, false, false}, // smstop
      Form{0xd503'437fU, false, false}, // smstart sm
      Form{0xd503'447fU, false, false}, // smstop za
      Form{0xd51b'4240U, false, false}, // msr svcr, x0
      Form{0xd53b'4240U, false, false}, // mrs x0, svcr
      Form{0xd53b'd0a1U, false, false}, // mrs x1, tpidr2_el0
      Form{0xd51b'd0a1U, false, false}, // msr tpidr2_el0, x1
      Form{0xe100'2043U, true, false},  // ldr za[w13, 3], [x2, #3, mul vl]
      Form{0xe120'03e0U, true, false},  // str za[w12, 0], [sp]
      Form{0xc008'00ffU, true, false},  // zero {za}
      Form{0xe11f'80a0U, true, true},   // ldr zt0, [x5]
      Form{0xe13f'83e0U, true, true},   // str zt0, [sp]
      Form{0xc048'0001U, true, true},   // zero {zt0}
  };
  for (const bool sme2 : {true, false}) {
    const auto host = sme_host(64, sme2);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (const auto &form : control)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const bool expected = (form.needs_sme2 && !sme2) ||
                              (form.needs_za && (mode & arm64::svcr_za) == 0U);
        if (illegal_in(form.word, mode) != expected) {
          std::fprintf(stderr, "%08x mode %u sme2 %d\n", form.word, mode,
                       sme2);
          require(false);
        }
      }
    /* MSR SVCR immediates outside SM, ZA and SMZA stay unallocated. */
    for (const std::uint32_t unallocated :
         {0xd503'407fU, 0xd503'417fU, 0xd503'487fU, 0xd503'4f7fU})
      require(illegal_in(unallocated, 0));
  }
  const auto bare = sme_host(0);
  const uml::cpu::HostCapabilitiesScope scope{bare};
  const std::array unknown{0xffff'ffffU};
  const auto reference = jit_test::compile_block(0x4f30'0000U, unknown);
  for (const auto &form : control) {
    const std::array program{form.word};
    require(jit_test::compile_block(0x4f30'0000U, program).words ==
            reference.words);
  }
}

/* The prologue's enter names the host mode the translation runs in. */
void enter_names_the_host_mode() {
  constexpr std::array program{add_x0_one, system_call};
  for (const bool sme2 : {true, false}) {
    const auto host = sme_host(64, sme2);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (std::uint8_t mode = arm64::svcr_sm; mode <= arm64::svcr_mask;
         mode = static_cast<std::uint8_t>(mode + 2U)) {
      const auto code =
          jit_test::compile_block_in_mode(0x4f40'0000U, program, mode).code;
      const auto expected = arm64::jit::jit_sme_host_mode(mode, sme2);
      const auto first = code.words.begin();
      const auto last =
          first + static_cast<std::ptrdiff_t>(code.internal_entry_word);
      require(std::find(first, last,
                        0xd280'0009U | (std::uint32_t{expected} << 5)) !=
              last);
    }
  }
}

/* Every row of the shared table, by its first word, translates exactly in
 * the modes the profile implements it in: the control core's forms and,
 * in streaming mode, every register-only and memory row of the SME1
 * profile, and outside streaming mode the SM = 0 vector-length family
 * (detail/sm0_vector_length.hpp), the one departure from the architecture.
 * Every other row (SME2's, the SVE rows that need FEAT_SVE outside
 * streaming mode, and those outside the profile) is the illegal exit.
 * Each translation also passes check_translation(). */
void rows_translate_exactly_where_implemented() {
  constexpr std::uint64_t pc = 0x4f50'0000U;
  for (const bool sme2 : {true, false}) {
    const auto host = sme_host(64, sme2);
    const uml::cpu::HostCapabilitiesScope scope{host};
    const auto features = arm64::implemented_sme_features(host);
    Checked checked;
    std::size_t streaming_rows = 0;
    std::size_t memory_rows = 0;
    for (const auto &row : arm64::detail::sme_encodings) {
      const auto word = row.value;
      if (arm64::detail::find_sme_encoding(word) != &row)
        continue;
      const auto *decoded =
          arm64::detail::decode_sme(word, features, host.sme_svl_bytes);
      const bool implemented =
          decoded != nullptr && arm64::jit::implements_sme_row(*decoded);
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const bool family = (mode & arm64::svcr_sm) == 0U &&
                            arm64::detail::sm0_family_word(word);
        const bool expected =
            family ||
            (implemented &&
             arm64::detail::sme_mode_permits(
                 *decoded, (mode & arm64::svcr_sm) != 0U,
                 (mode & arm64::svcr_za) != 0U));
        if (illegal_in(word, mode) == expected) {
          std::fprintf(stderr, "row %04x %08x mode %u sme2 %d: expected "
                       "%s\n", row.form, word, mode, sme2,
                       expected ? "a translation" : "the illegal exit");
          require(false);
        }
        const bool counted = expected && mode == arm64::svcr_mask &&
                             !arm64::jit::implements_sme_form(row.form);
        streaming_rows += counted && row.memory == 0U;
        memory_rows += counted && row.memory != 0U;
      }
      const std::array program{word, system_call};
      char label[32];
      std::snprintf(label, sizeof label, "row %08x", word);
      check_program(pc, program, label, checked);
    }
    std::fprintf(stderr, "sme2 %d: %zu register-only and %zu memory rows "
                 "translate at SM = ZA = 1\n", sme2, streaming_rows,
                 memory_rows);
    require(streaming_rows > 900U);
    require(memory_rows > 400U);
  }
}

/* The SM = 0 vector-length family translates as its base word: the code
 * of a family word is the code of the ADD, SUB, MOVZ, MOVN or NOP that
 * detail::sm0_vector_length_equivalent() names, in every mode without
 * PSTATE.SM, at every SVL and with SME2 or without, and in streaming mode
 * it stays the SVL-based form. Without SME the SME-only translation does
 * not apply: the words are the illegal exit. */
void vector_length_family_translates_as_its_base_word() {
  constexpr std::uint64_t pc = 0x4f58'0000U;
  const auto words = uml::emulator::tests::sm0_vector_length::words();
  for (const bool sme2 : {true, false})
    for (const unsigned svl : {16U, 64U, 256U}) {
      const auto host = sme_host(svl, sme2);
      const uml::cpu::HostCapabilitiesScope scope{host};
      std::size_t checked = 0;
      for (std::size_t index = 0; index < words.size(); index += 7U) {
        const auto word = words[index];
        const auto base = arm64::detail::sm0_vector_length_equivalent(word);
        require(base.has_value());
        for (const std::uint8_t mode : {std::uint8_t{0}, arm64::svcr_za}) {
          const std::array program{word, system_call};
          const std::array reference{*base, system_call};
          const auto code =
              jit_test::compile_block_in_mode(pc, program, mode).code;
          require(!code.guest_illegal);
          if (code.words !=
              jit_test::compile_block_in_mode(pc, reference, mode).code.words) {
            std::fprintf(stderr, "%08x mode %u: not the code of %08x\n", word,
                         mode, *base);
            require(false);
          }
        }
        for (const std::uint8_t mode :
             {arm64::svcr_sm, static_cast<std::uint8_t>(arm64::svcr_mask)}) {
          const std::array program{word, system_call};
          const std::array reference{*base, system_call};
          require(jit_test::compile_block_in_mode(pc, program, mode)
                      .code.words !=
                  jit_test::compile_block_in_mode(pc, reference, mode)
                      .code.words);
        }
        ++checked;
      }
      require(checked > 600U);
    }
  /* Without SME the words are undefined, as is any SVE word. */
  const auto bare = sme_host(0);
  const uml::cpu::HostCapabilitiesScope scope{bare};
  for (const auto word : {0x0420'e3e0U, 0x04bf'5000U, 0x0430'e3e0U})
    require(illegal_in(word, 0));
}

/* The spill words translate outside streaming mode, at every SVL, with SME2
 * or without, and with ZA enabled or not: a vector register Z reads or
 * writes V and so the block accesses the FP state, a predicate register does
 * not. In streaming mode they keep the SVE rows' streaming translation;
 * without SME they are the illegal exit, as is every other SVE load and
 * store. */
void spill_words_translate_as_memory_accesses() {
  constexpr std::uint64_t pc = 0x4f58'8000U;
  const auto spills = uml::emulator::tests::sm0_vector_length::spills();
  for (const bool sme2 : {true, false})
    for (const unsigned svl : {16U, 64U, 256U}) {
      const auto host = sme_host(svl, sme2);
      const uml::cpu::HostCapabilitiesScope scope{host};
      std::size_t checked = 0;
      for (std::size_t index = 0; index < spills.size(); index += 5U) {
        const auto &spill = spills[index];
        const auto word = spill.word();
        for (const std::uint8_t mode : {std::uint8_t{0}, arm64::svcr_za}) {
          const std::array program{word, system_call};
          const auto code =
              jit_test::compile_block_in_mode(pc, program, mode).code;
          require(!code.guest_illegal);
          require(code.accesses_fp_state == !spill.predicate);
          require((code.memory_write_mask != 0U ||
                   code.direct_memory_write_mask != 0U) == spill.store);
        }
        ++checked;
      }
      require(checked > 150U);
    }
  const auto bare = sme_host(0);
  const uml::cpu::HostCapabilitiesScope scope{bare};
  for (const auto &spill : spills)
    require(illegal_in(spill.word(), 0));
  /* Every other SVE load and store stays the illegal exit on an SME CPU
   * without SVE: contiguous, structure, gather and replicating forms. */
  const auto host = sme_host(64, true);
  const uml::cpu::HostCapabilitiesScope sme_scope{host};
  for (const std::uint32_t word :
       {0xa400'a000U /* LD1B */, 0xa540'a000U /* LD1H */,
        0xe400'e000U /* ST1B */, 0xa4a0'2000U /* LD1RQW */,
        0x8440'c000U /* LD1RB */, 0xc400'c000U /* gather */,
        0x8500'0000U /* PRFB-adjacent */, 0xe590'4000U /* STR Z reserved */})
    if (!arm64::detail::sm0_family_word(word))
      require(illegal_in(word, 0));
}

/* Every FP&SIMD class, by its first word (with Rt2 = 1, since a load
 * pair naming one register twice is UNDEFINED), translates in SM=1
 * exactly when the profile marks it FP-legal and it processes data or is
 * a SIMD&FP load or store decode_streaming_fp_memory() accepts. AdvSIMD
 * is the illegal exit, and so are the FEAT_LSFE and FEAT_LSUI loads and
 * stores, which the virtual CPU lacks. Every word of an accepted memory
 * class decodes, and none of a rejected one. */
void fp_simd_classes_translate_where_streaming_runs_them() {
  constexpr std::uint64_t pc = 0x4f60'0000U;
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  Checked checked;
  std::size_t translated = 0;
  std::size_t memory_classes = 0;
  std::size_t rejected_memory = 0;
  std::uint64_t state = 0x9e37'79b9'7f4a'7c15ULL;
  for (const auto &row : arm64::detail::fp_simd_streaming_classes) {
    const auto word = row.value | (~row.mask & 0x0400U);
    if (streaming_fp(word) == row.advsimd)
      continue; // an earlier row claims the word
    const bool memory = (word & 0x0e00'0000U) == 0x0c00'0000U;
    const bool decodes =
        arm64::jit::decode_streaming_fp_memory(word).has_value();
    const bool expected = !row.advsimd && (!memory || decodes);
    if (memory && !row.advsimd) {
      memory_classes += decodes;
      rejected_memory += !decodes;
      for (int sample = 0; sample < 256; ++sample) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto other =
            row.value | (static_cast<std::uint32_t>(state >> 32) & ~row.mask);
        if (streaming_fp(other) == row.advsimd)
          continue;
        const auto decoded = arm64::jit::decode_streaming_fp_memory(other);
        const bool same_pair = decoded && decoded->registers == 2U &&
                               decoded->transfer == decoded->transfer2;
        const bool pair_load_same =
            !decoded && ((other >> 10) & 31U) == (other & 31U) &&
            (other & 0x3a40'0000U) == 0x2840'0000U;
        if (decoded.has_value() != decodes && !pair_load_same) {
          std::fprintf(stderr, "FP&SIMD class %u: %08x decodes %d, %08x "
                       "decodes %d\n", row.form, word, decodes, other,
                       decoded.has_value());
          require(false);
        }
        require(!same_pair || !decoded->load);
      }
    }
    for (const std::uint8_t mode :
         {arm64::svcr_sm, static_cast<std::uint8_t>(arm64::svcr_mask)})
      if (illegal_in(word, mode) == expected) {
        std::fprintf(stderr, "FP&SIMD class %u %08x mode %u: expected %s\n",
                     row.form, word, mode,
                     expected ? "a translation" : "the illegal exit");
        require(false);
      }
    translated += expected;
    const std::array program{word, system_call};
    char label[32];
    std::snprintf(label, sizeof label, "class %08x", word);
    check_program(pc, program, label, checked);
  }
  std::fprintf(stderr, "%zu FP&SIMD classes translate in streaming mode, "
               "%zu of them loads and stores; %zu load and store classes "
               "do not\n", translated, memory_classes, rejected_memory);
  require(translated > 500U);
  require(memory_classes == 89U);
}

/* The SVE words whose rows can raise a floating-point exception, and so
 * run with the FPSR capture, are exactly those named for floating point
 * but for the sign and move forms: a regenerated table cannot drop one. */
void fp_capture_follows_form_names(const char *bindings) {
  const auto host = sme_host(64);
  const auto features = arm64::implemented_sme_features(host);
  std::size_t raising = 0;
  for (const auto &witness : witnesses(bindings)) {
    if (witness.section != "SVE")
      continue;
    const auto *row = arm64::detail::decode_sme(witness.word, features,
                                                host.sme_svl_bytes);
    if (row == nullptr || !arm64::jit::implements_sme_row(*row) ||
        arm64::jit::implements_sme_form(row->form))
      continue;
    const auto &name = witness.form;
    const auto starts = [&name](const char *prefix) {
      return name.rfind(prefix, 0) == 0;
    };
    const bool named = (starts("F") || starts("BF") || starts("SCVTF") ||
                        starts("UCVTF")) &&
                       name != "FABS" && name != "FNEG" && name != "FCPY" &&
                       name != "FDUP" && !starts("FMOV") &&
                       !starts("FIRSTP");
    if (named != arm64::jit::sve_raises_fp_exceptions(witness.word)) {
      std::fprintf(stderr, "%s %s %08x: capture %d\n", witness.id.c_str(),
                   name.c_str(), witness.word, !named);
      require(false);
    }
    raising += named;
  }
  std::fprintf(stderr, "%zu SVE forms capture FPSR\n", raising);
  require(raising > 60U);
}

/* ORR, EOR and AND (immediate) and DUPM translate in streaming mode for
 * every bitmask immediate but the 512 per form DecodeBitMasks reserves:
 * N = 1 with imms all ones, N = 0 with at most one zero in imms. Those
 * are the illegal exit, where running the word would raise SIGILL in the
 * host. */
/* FEAT_MOPS's CPY words set bit 26, as a SIMD&FP load or store does, but
 * are no FP&SIMD instruction: every CPY, CPYF and SET word, in each size,
 * stage and option, translates in streaming mode exactly where it does at
 * SVCR 0. */
void memory_copy_set_follows_mode_zero() {
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  std::size_t translated = 0;
  for (std::uint32_t size = 0; size < 4U; ++size)
    for (std::uint32_t general = 0; general < 2U; ++general)
      for (std::uint32_t op1 = 0; op1 < 4U; ++op1)
        for (std::uint32_t op2 = 0; op2 < 16U; ++op2) {
          const auto word = (size << 30) | 0x1900'0400U | (general << 26) |
                            (op1 << 22) | (1U << 16) | (op2 << 12) |
                            (2U << 5);
          const bool expected = illegal_in(word, 0);
          translated += !expected;
          for (const std::uint8_t mode :
               {arm64::svcr_sm, static_cast<std::uint8_t>(arm64::svcr_mask)})
            if (illegal_in(word, mode) != expected) {
              std::fprintf(stderr, "MOPS %08x mode %u: expected %s\n", word,
                           mode,
                           expected ? "the illegal exit" : "a translation");
              require(false);
            }
        }
  require(translated != 0U);
}

void reserved_bitmask_immediates_are_illegal() {
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  for (const std::uint32_t form :
       {0x0500'0000U, 0x0540'0000U, 0x0580'0000U, 0x05c0'0000U}) {
    std::size_t reserved = 0;
    for (std::uint32_t immediate = 0; immediate < 0x2000U; ++immediate) {
      const auto word = form | (immediate << 5) | (immediate % 31U);
      const unsigned imms = immediate & 63U;
      const bool expected = (immediate & 0x1000U) != 0U
                                ? imms == 63U
                                : std::popcount(imms) >= 5;
      reserved += expected;
      for (const std::uint8_t mode :
           {arm64::svcr_sm, static_cast<std::uint8_t>(arm64::svcr_mask)})
        if (illegal_in(word, mode) != expected) {
          std::fprintf(stderr, "%08x mode %u: expected %s\n", word, mode,
                       expected ? "the illegal exit" : "a translation");
          require(false);
        }
    }
    require(reserved == 512U);
  }
}

void population_count_needs_no_advanced_simd() {
  for (const std::uint64_t value :
       {0ULL, 1ULL, 0x8000'0000'0000'0000ULL, ~0ULL, 0x5555'5555'5555'5555ULL,
        0x0123'4567'89ab'cdefULL, 0xffff'ffff'0000'0001ULL,
        0x0000'0001'ffff'ffffULL}) {
    require(jit_test::gpr_population_count_for_testing(true, value) ==
            static_cast<std::uint64_t>(std::popcount(value)));
    require(jit_test::gpr_population_count_for_testing(false, value) ==
            static_cast<std::uint64_t>(
                std::popcount(static_cast<std::uint32_t>(value))));
  }
  std::uint64_t state = 0x243f'6a88'85a3'08d3ULL;
  for (int round = 0; round < 10'000; ++round) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    require(jit_test::gpr_population_count_for_testing(true, state) ==
            static_cast<std::uint64_t>(std::popcount(state)));
    require(jit_test::gpr_population_count_for_testing(false, state) ==
            static_cast<std::uint64_t>(
                std::popcount(static_cast<std::uint32_t>(state))));
  }
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(argc == 2);
  streaming_translations_hold_no_fp_simd_word(argv[1]);
  translations_do_not_depend_on_the_svl(argv[1]);
  control_core_follows_profile_and_mode();
  rows_translate_exactly_where_implemented();
  vector_length_family_translates_as_its_base_word();
  spill_words_translate_as_memory_accesses();
  fp_simd_classes_translate_where_streaming_runs_them();
  fp_capture_follows_form_names(argv[1]);
  reserved_bitmask_immediates_are_illegal();
  memory_copy_set_follows_mode_zero();
  enter_names_the_host_mode();
  if (arm64::native_jit_available())
    population_count_needs_no_advanced_simd();
  return 0;
}
