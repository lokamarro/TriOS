// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SME in the arm64 JIT: the control core and the register-only forms. Every
 * size and layout that depends on the streaming vector length is read at
 * run time, so a translation is the same for every SVL: with RDSVL and
 * ADDSVL in SM=1 translations, from CpuState::sme_svl in the others.
 *
 * SM=1 translations execute the ZA and ZT0 forms, the register-only forms
 * and the memory forms natively on the guest's resident state, with
 * only their general register fields re-encoded and a memory form's base
 * rebased into the host page. SM=0 translations run with host SME off and
 * contain no SVE or SME instruction: they copy between guest memory and the
 * register file with GPRs.
 */
#include "jit_compiler.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"

#include <algorithm>
#include <bit>
#include <optional>
#include <utility>

namespace uml::cpu::arm64 {

namespace {

namespace forms = jit::sme_forms;

[[nodiscard]] constexpr std::uint32_t rdsvl(unsigned destination,
                                            unsigned multiple) noexcept {
  return 0x04bf'5800U | ((multiple & 63U) << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t addsvl(unsigned destination,
                                             unsigned source,
                                             unsigned multiple) noexcept {
  return 0x0420'5800U | (source << 16) | ((multiple & 63U) << 5) |
         destination;
}

/* LDR or STR ZA[W12, 0], [Xbase]. */
[[nodiscard]] constexpr std::uint32_t za_vector_transfer(bool load,
                                                         unsigned base) noexcept {
  return (load ? 0xe100'0000U : 0xe120'0000U) | (base << 5);
}

/* LDR or STR ZT0, [Xbase]. */
[[nodiscard]] constexpr std::uint32_t zt0_transfer(bool load,
                                                   unsigned base) noexcept {
  return (load ? 0xe11f'8000U : 0xe13f'8000U) | (base << 5);
}

/* LDP or STP of X0 and X1 at [Xbase], #16 (post-index). */
[[nodiscard]] constexpr std::uint32_t pair_post_index(bool load,
                                                      unsigned base) noexcept {
  return (load ? 0xa8c1'0400U : 0xa881'0400U) | (base << 5);
}

/* MADD Xd, Xn, Xm, Xa. */
[[nodiscard]] constexpr std::uint32_t multiply_add(unsigned destination,
                                                   unsigned left,
                                                   unsigned right,
                                                   unsigned addend) noexcept {
  return 0x9b00'0000U | (right << 16) | (addend << 10) | (left << 5) |
         destination;
}

/* Xd = SVL, from RDSVL in a streaming translation and from CpuState in the
 * others, where the host's SME is off. */
void load_vector_length(Arm64Emitter &emitter, bool streaming,
                        unsigned destination) {
  if (streaming)
    emitter.emit(rdsvl(destination, 1));
  else
    emitter.load_w(destination, 15, offsetof(CpuState, sme_svl));
}

static_assert(offsetof(CpuState, sme_svl) % 4U == 0U);
static_assert(detail::find_sme_encoding(rdsvl(1, 1))->form == forms::rdsvl);
static_assert(detail::find_sme_encoding(addsvl(8, 8, 15))->form ==
              forms::addsvl);
static_assert(detail::find_sme_encoding(za_vector_transfer(true, 8))->form ==
              forms::ldr_za);
static_assert(detail::find_sme_encoding(za_vector_transfer(false, 8))->form ==
              forms::str_za);
static_assert(detail::find_sme_encoding(zt0_transfer(true, 8))->form ==
              forms::ldr_zt0);
static_assert(detail::find_sme_encoding(zt0_transfer(false, 8))->form ==
              forms::str_zt0);

constexpr std::uint32_t clear_fpsr = 0xd51b'443fU;    // msr fpsr, xzr
constexpr std::uint32_t read_fpsr_x11 = 0xd53b'442bU; // mrs x11, fpsr
constexpr std::uint32_t save_flags_x16 = 0xd53b'4210U;    // mrs x16, nzcv
constexpr std::uint32_t restore_flags_x16 = 0xd51b'4210U; // msr nzcv, x16
constexpr unsigned fpsr_qc_bit = 27;

/* LDR or STR Zt or Pt, [Xbase, #imm, MUL VL]. */
[[nodiscard]] constexpr std::uint32_t register_fill(bool load, bool predicate,
                                                    unsigned base,
                                                    unsigned imm,
                                                    unsigned reg) noexcept {
  return (load ? 0x8580'0000U : 0xe580'0000U) | (predicate ? 0U : 0x4000U) |
         ((imm >> 3) << 16) | ((imm & 7U) << 10) | (base << 5) | reg;
}

/* The register file holds Z_n at n * SVL and P_n at 32 * SVL + n * SVL / 8,
 * which is 16 * SVL further on plus (128 + n) predicate lengths: the
 * immediate of register_fill() for a base that ADDSVL #16 moved. */
constexpr unsigned predicate_base_svl = 16;
constexpr unsigned predicate_fill_bias = 128;
static_assert(sme_p_offset_vl == 32U);
static_assert((sme_p_offset_vl - predicate_base_svl) * 8U ==
              predicate_fill_bias);
static_assert(register_fill(false, false, 16, 5, 5) == 0xe580'5605U);
static_assert(register_fill(true, false, 11, 31, 31) == 0x8583'5d7fU);
static_assert(register_fill(false, true, 16, 131, 3) == 0xe590'0e03U);
static_assert(register_fill(true, true, 11, 143, 15) == 0x8591'1d6fU);

/* The single field of a register operand numbered by one field. */
[[nodiscard]] constexpr bool one_field(const detail::SmeOperand &operand,
                                       unsigned width) noexcept {
  return operand.segment_count == 1U && operand.segments[0].field &&
         operand.segments[0].width == width;
}

} // namespace

std::optional<JitCompiler::Action>
JitCompiler::emit_sme_system(std::uint32_t instruction, std::uint64_t pc) {
  /* MSR SVCRSM, SVCRZA or SVCRSMZA, #imm is MSR (immediate) with op1 3 and
   * op2 3; the others are MRS and MSR (register). */
  const bool svcr_immediate = (instruction & 0xffff'f0ffU) == 0xd503'407fU;
  const auto system = instruction & 0xffff'ffe0U;
  if (!svcr_immediate && system != 0xd53b'4240U && system != 0xd51b'4240U &&
      system != 0xd53b'd0a0U && system != 0xd51b'd0a0U)
    return std::nullopt;
  if (!implements_sme(host_capabilities()))
    return std::nullopt;
  const unsigned transfer = instruction & 31U;
  const auto write_destination = [&](auto emit_value) {
    if (transfer == 31U)
      return;
    const auto destination = write_native_gpr(transfer, false, false, 8);
    emit_value(destination.host);
    commit_native_gpr(destination, transfer, false);
  };
  if (system == 0xd53b'4240U) {
    /* MRS SVCR: the translation's mode is the guest's. */
    write_destination([&](unsigned destination) {
      emitter_.load_constant(destination, sme_mode());
    });
    return Action::keep_going;
  }
  /* TPIDR2_EL0 is the guest's own, never the host register, which belongs
   * to the host's SME ABI. */
  if (system == 0xd53b'd0a0U) {
    write_destination([&](unsigned destination) {
      emitter_.load_x(destination, 15, offsetof(CpuState, tpidr2));
    });
    return Action::keep_going;
  }
  if (system == 0xd51b'd0a0U) {
    const auto source = read_native_gpr(transfer, false, 8);
    emitter_.store_x(source.host, 15, offsetof(CpuState, tpidr2));
    return Action::keep_going;
  }
  if (svcr_immediate) {
    /* CRm 001x, 010x or 011x selects SM, ZA or both; the rest are
     * unallocated. */
    const unsigned field = (instruction >> 8) & 15U;
    if (field < 2U || field > 7U)
      return Action::unsupported;
  }
  /* A write of SVCR ends the block: its successor runs in the new mode. */
  emit_terminal_helper(
      reinterpret_cast<const void *>(&uml_arm64_jit_write_svcr), instruction,
      pc, count_ + 1U);
  return Action::terminate;
}

JitCompiler::Action JitCompiler::emit_sme_instruction(std::uint32_t instruction,
                                                      std::uint64_t pc) {
  const auto &host = host_capabilities();
  const auto *row = arm64::detail::decode_sme(
      instruction, implemented_sme_features(host), host.sme_svl_bytes);
  const auto mode = sme_mode();
  if (row == nullptr || !jit::implements_sme_row(*row) ||
      !arm64::detail::sme_mode_permits(*row, (mode & svcr_sm) != 0U,
                                       (mode & svcr_za) != 0U))
    return Action::unsupported;
  const bool streaming = streaming_translation();
  const auto form = row->form;
  if (!jit::implements_sme_form(form)) {
    /* A register-only or memory form: every one needs streaming mode. */
    if (!streaming)
      return Action::unsupported;
    if (row->memory != 0U)
      return emit_streaming_memory(*row, instruction, pc)
                 ? Action::keep_going
                 : Action::unsupported;
    if (form == forms::movprfx || form == forms::movprfx_predicated) {
      emit_movprfx(instruction, form == forms::movprfx_predicated);
      return Action::keep_going;
    }
    const auto plan = plan_sme_row(*row, instruction);
    if (!plan)
      return Action::unsupported;
    emit_streaming_word(*plan);
    return Action::keep_going;
  }
  if (form == forms::rdsvl || form == forms::addsvl ||
      form == forms::addspl) {
    emit_sme_vector_length(instruction, form == forms::rdsvl);
    return Action::keep_going;
  }
  if (form == forms::ldr_za || form == forms::str_za) {
    emit_sme_array_vector(instruction, pc, form == forms::ldr_za);
    return Action::keep_going;
  }
  if (form == forms::ldr_zt0 || form == forms::str_zt0) {
    emit_sme_zt0_transfer(instruction, pc, form == forms::ldr_zt0);
    return Action::keep_going;
  }
  if (streaming) {
    /* ZERO ZA and ZERO ZT0 name no register; ZA is resident. */
    emitter_.emit(instruction);
    return Action::keep_going;
  }
  if (form == forms::zero_zt0) {
    emit_sme_zt0_address();
    for (unsigned offset = 0; offset < 64U; offset += 16U)
      emitter_.emit(0xa900'0000U | ((offset / 8U) << 15) | (31U << 10) |
                    (9U << 5) | 31U); // stp xzr, xzr, [x9, #offset]
    return Action::keep_going;
  }
  /* ZERO ZA in an SM=0 translation: rare (ZA was just enabled), so the
   * register file's tiles are cleared by the helper. */
  emit_terminal_helper(
      reinterpret_cast<const void *>(&uml_arm64_jit_execute_sme), instruction,
      pc, count_ + 1U);
  return Action::terminate;
}

void JitCompiler::emit_sme_vector_length(std::uint32_t instruction,
                                         bool read) {
  const unsigned destination = instruction & 31U;
  const bool streaming = streaming_translation();
  if (read && destination == 31U)
    return; // RDSVL Xd, #imm: Rd 31 is XZR.
  if (!streaming) {
    /* x10 = SVL, or SVL / 8 for ADDSPL; x11 = the signed multiple. */
    load_vector_length(emitter_, false, 10);
    if ((instruction & 0x0040'0000U) != 0U)
      emitter_.ubfm(10, 10, 3, 63); // lsr x10, x10, #3
    emitter_.load_constant(
        11, static_cast<std::uint64_t>(sign_extend((instruction >> 5) & 63U, 6)));
  }
  if (read) {
    const auto native = write_native_gpr(destination, false, false, 9);
    if (streaming)
      emitter_.emit((instruction & ~31U) | native.host);
    else
      emitter_.emit(multiply_add(native.host, 10, 11, 31));
    commit_native_gpr(native, destination, false);
    return;
  }
  /* ADDSVL and ADDSPL Xd|SP, Xn|SP, #imm. */
  const unsigned source = (instruction >> 16) & 31U;
  const auto native_source = read_native_gpr(source, true, 8);
  const auto native_destination =
      write_native_gpr(destination, true, source == destination, 9);
  if (streaming)
    emitter_.emit((instruction & ~((31U << 16) | 31U)) |
                  (native_source.host << 16) | native_destination.host);
  else
    emitter_.emit(
        multiply_add(native_destination.host, 10, 11, native_source.host));
  commit_native_gpr(native_destination, destination, true);
}

void JitCompiler::emit_sme_array_vector(std::uint32_t instruction,
                                        std::uint64_t pc, bool load) {
  /* LDR and STR ZA[Wv, #off4], [Xn|SP, #off4, MUL VL]: ZA vector
   * (Wv + off4) MOD SVL, at Xn + off4 * SVL, with Wv one of W12-W15. */
  const unsigned base = (instruction >> 5) & 31U;
  const unsigned offset = instruction & 15U;
  const unsigned index = 12U + ((instruction >> 13) & 3U);
  if (!load)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  const auto fault =
      cold_exit(instruction, pc, ColdPath::Action::memory_fault);
  load_gpr(8, base, true);
  /* A misaligned SP takes the helper, which reports the alignment fault. */
  if (base == 31U)
    emit_misalignment_check(8, 16, slow);
  const bool streaming = streaming_translation();
  load_vector_length(emitter_, streaming, 1);
  if (offset != 0U) {
    if (streaming) {
      emitter_.emit(addsvl(8, 8, offset));
    } else {
      emitter_.load_constant(2, offset);
      emitter_.emit(multiply_add(8, 1, 2, 8));
    }
  }
  emit_untag_data_address(8);
  /* The vector's SVL bytes must lie in one page: SVL is below a page, so
   * the page bit of (address & page_mask) + SVL - 1 is the test. A
   * crossing vector takes the helper, which translates both pages first. */
  emitter_.ubfm(0, 8, 0, page_shift - 1U);
  emitter_.add_shifted(0, 0, 1, 0);
  emitter_.subtract_immediate(0, 0, 1);
  emitter_.test_zero(slow, 0, page_shift, true);
  emit_translate(8, load ? Access::read : Access::write, slow, fault, 1);
  if (!load) {
    load_vector_length(emitter_, streaming, 10);
    track_direct_store_register(8, 10, count_);
  }
  if (streaming) {
    /* x12 holds no guest register in a streaming translation. */
    load_gpr(12, index, false);
    if (offset != 0U)
      emitter_.emit(0x1100'0000U | (offset << 10) | (12U << 5) |
                    12U); // add w12, w12, #offset
    emitter_.emit(za_vector_transfer(load, 8));
  } else {
    /* x9 = the register file's ZA vector (34 + vector) * SVL. */
    emitter_.load_x(9, 15, offsetof(CpuState, sme_registers));
    load_gpr(11, index, false);
    if (offset != 0U)
      emitter_.add_immediate(11, 11, offset);
    load_vector_length(emitter_, false, 10);
    emitter_.subtract_immediate(16, 10, 1);
    emitter_.emit(0x8a10'016bU); // and x11, x11, x16
    emitter_.add_immediate(11, 11, static_cast<unsigned>(sme_za_offset_vl));
    emitter_.emit(multiply_add(9, 11, 10, 9));
    /* SVL bytes, 16 at a time, from x8 to x9 or back. */
    const auto loop = emitter_.make_label();
    emitter_.bind(loop);
    emitter_.emit(pair_post_index(true, load ? 8U : 9U));
    emitter_.emit(pair_post_index(false, load ? 9U : 8U));
    emitter_.subtract_immediate(10, 10, 16);
    emitter_.compare_zero(loop, 10, true, true);
  }
}

void JitCompiler::emit_sme_zt0_address() {
  /* ZT0 follows the SVL ZA vectors: 34 * SVL + SVL * SVL. */
  emitter_.load_x(9, 15, offsetof(CpuState, sme_registers));
  load_vector_length(emitter_, false, 10);
  emitter_.add_immediate(11, 10, static_cast<unsigned>(sme_za_offset_vl));
  emitter_.emit(multiply_add(9, 11, 10, 9));
}

void JitCompiler::emit_sme_zt0_transfer(std::uint32_t instruction,
                                        std::uint64_t pc, bool load) {
  /* LDR and STR ZT0, [Xn|SP]: 64 bytes. */
  const unsigned base = (instruction >> 5) & 31U;
  if (!load)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  const auto fault =
      cold_exit(instruction, pc, ColdPath::Action::memory_fault);
  load_gpr(8, base, true);
  if (base == 31U)
    emit_misalignment_check(8, 16, slow);
  emit_untag_data_address(8);
  emit_translate(8, load ? Access::read : Access::write, slow, fault,
                 sme_zt0_bytes);
  if (!load)
    track_direct_store(8, sme_zt0_bytes);
  if (streaming_translation()) {
    emitter_.emit(zt0_transfer(load, 8));
  } else {
    emit_sme_zt0_address();
    for (unsigned pair = 0; pair < sme_zt0_bytes / 16U; ++pair) {
      emitter_.emit(pair_post_index(true, load ? 8U : 9U));
      emitter_.emit(pair_post_index(false, load ? 9U : 8U));
    }
  }
}

std::optional<JitCompiler::StreamingWord>
JitCompiler::plan_sme_row(const detail::SmeEncoding &row,
                          std::uint32_t instruction) {
  using Saved = StreamingWord::Saved;
  if (jit::sme_word_reserved(row, instruction))
    return std::nullopt;
  StreamingWord plan{.word = instruction};
  const auto *operands = detail::sme_encoding_operands_of(row);
  unsigned destinations = 0;
  std::optional<detail::SmeOperandKind> destination_kind;
  unsigned destination = 0;
  unsigned destination_count = 1;
  /* Bit p set: P_p is a source. */
  unsigned predicate_sources = 0;
  for (unsigned index = 0; index < row.operand_count; ++index) {
    const auto &operand = operands[index];
    const auto number = detail::sme_operand_number(operand, instruction);
    const bool at_zero = std::any_of(
        operand.segments.begin(),
        operand.segments.begin() + operand.segment_count,
        [](const detail::SmeSegment &segment) {
          return segment.field && segment.low == 0U;
        });
    switch (operand.kind) {
    case detail::SmeOperandKind::za_tile:
      continue;
    case detail::SmeOperandKind::gpr:
    case detail::SmeOperandKind::gpr_or_sp:
      /* A re-encoded field must stay inside the row: the row may fix no
       * bit of it. */
      if (one_field(operand, 5)) {
        if (plan.gpr_count == plan.gprs.size() ||
            ((row.mask >> operand.segments[0].low) & 31U) != 0U)
          return std::nullopt;
        destinations += at_zero ? 1U : 0U;
        plan.gprs[plan.gpr_count++] = {
            .low = operand.segments[0].low,
            .guest = static_cast<std::uint8_t>(number),
            .stack_pointer =
                operand.kind == detail::SmeOperandKind::gpr_or_sp,
            .destination = at_zero,
        };
        continue;
      }
      /* A slice index: a constant prefix and a field, naming W12-W15 in
       * the SME1 forms, which run with the index in W12. */
      if (operand.segment_count == 2U && !operand.segments[0].field &&
          operand.segments[1].field && !plan.slice) {
        const unsigned width = operand.segments[1].width;
        if ((12U >> width) != operand.segments[0].constant ||
            (12U & ((1U << width) - 1U)) != 0U ||
            ((row.mask >> operand.segments[1].low) &
             ((1U << width) - 1U)) != 0U)
          return std::nullopt;
        plan.slice = true;
        plan.slice_low = operand.segments[1].low;
        plan.slice_width = static_cast<std::uint8_t>(width);
        plan.slice_guest = static_cast<std::uint8_t>(number);
        continue;
      }
      return std::nullopt;
    case detail::SmeOperandKind::z:
    case detail::SmeOperandKind::v:
    case detail::SmeOperandKind::p:
      if (at_zero) {
        ++destinations;
        destination_kind = operand.kind;
        destination = number;
        destination_count = operand.count;
      } else if (operand.kind == detail::SmeOperandKind::p) {
        for (unsigned reg = 0; reg < operand.count; ++reg)
          predicate_sources |= 1U << ((number + reg * operand.stride) & 15U);
      }
      continue;
    }
  }
  if (destinations > 1U)
    return std::nullopt;
  const bool predicate_destination =
      destination_kind == detail::SmeOperandKind::p;
  plan.reads_flags = plan.writes_flags =
      jit::sme_row_uses_flags(row, predicate_destination);
  plan.raises = (row.form & detail::sme_form_sme_section) == 0U &&
                jit::sve_raises_fp_exceptions(instruction);
  if (plan.raises && destination_kind) {
    /* A rerun must see the sources the first run saw: a Z destination is
     * read by every merging and destructive form, a P destination only
     * when it is also a source. */
    if (destination_count != 1U)
      return std::nullopt;
    if (!predicate_destination)
      plan.saved = Saved::z;
    else if ((predicate_sources & (1U << destination)) != 0U)
      plan.saved = Saved::p;
    plan.saved_register = static_cast<std::uint8_t>(destination);
  }
  return plan;
}

std::optional<JitCompiler::StreamingWord>
JitCompiler::plan_streaming_fp_simd(std::uint32_t instruction) {
  /* The SIMD&FP loads and stores wait for the memory forms. */
  if (detail::classify_fp_simd_streaming(instruction) !=
          detail::FpSimdStreaming::fp ||
      !floating_or_vector_primary_class(instruction))
    return std::nullopt;
  StreamingWord plan{.word = instruction};
  /* Which way a general register goes, if one does: Rd or Rn, where 31
   * is the zero register. */
  enum class General { none, destination, source };
  auto general = General::none;
  bool vector_destination = true;
  plan.raises = true;
  if ((instruction & 0x9fe0'8400U) == 0x0e00'0400U) {
    /* SMOV and UMOV of element 0. */
    general = General::destination;
    plan.raises = false;
  } else if ((instruction & 0x5f00'0000U) == 0x1e00'0000U) {
    /* Scalar floating point and conversions (floatdp3 has bit 24). */
    const unsigned opcode = (instruction >> 16) & 7U;
    if ((instruction & 0x0020'0000U) == 0U) {
      /* Fixed-point conversion: SCVTF and UCVTF read Rn. */
      general = opcode == 2U || opcode == 3U ? General::source
                                             : General::destination;
    } else {
      switch ((instruction >> 10) & 3U) {
      case 1: // conditional compare
        vector_destination = false;
        plan.reads_flags = plan.writes_flags = true;
        break;
      case 2: // two sources
        break;
      case 3: // conditional select
        plan.reads_flags = true;
        plan.raises = false;
        break;
      default:
        if ((instruction & 0x1c00U) == 0x1000U) {
          plan.raises = false; // FMOV (scalar, immediate)
        } else if ((instruction & 0x3c00U) == 0x2000U) {
          vector_destination = false; // compare
          plan.writes_flags = true;
        } else if ((instruction & 0x7c00U) == 0x4000U) {
          /* One source: FMOV, FABS and FNEG raise nothing. */
          plan.raises = ((instruction >> 15) & 63U) > 2U;
        } else if ((instruction & 0xfc00U) == 0U) {
          /* Integer conversion; FMOV (general) raises nothing. */
          general = opcode == 2U || opcode == 3U || opcode == 7U
                        ? General::source
                        : General::destination;
          plan.raises = opcode < 6U;
        } else {
          return std::nullopt;
        }
      }
    }
  }
  if (general == General::destination) {
    plan.gprs[plan.gpr_count++] = {.low = 0,
                                   .guest = static_cast<std::uint8_t>(
                                       instruction & 31U),
                                   .stack_pointer = false,
                                   .destination = true};
    vector_destination = false;
  } else if (general == General::source) {
    plan.gprs[plan.gpr_count++] = {.low = 5,
                                   .guest = static_cast<std::uint8_t>(
                                       (instruction >> 5) & 31U),
                                   .stack_pointer = false,
                                   .destination = false};
  }
  /* A rerun must see the sources the first run saw: save the destination
   * when one of the register fields it can read names it (the fields at
   * 5, 16 and 10; in a form where one is not a register, at worst an
   * unneeded save). */
  const unsigned destination = instruction & 31U;
  if (plan.raises && vector_destination && general == General::none &&
      (destination == ((instruction >> 5) & 31U) ||
       destination == ((instruction >> 16) & 31U) ||
       destination == ((instruction >> 10) & 31U))) {
    plan.saved = StreamingWord::Saved::z;
    plan.saved_register = static_cast<std::uint8_t>(destination);
  }
  return plan;
}

void JitCompiler::emit_streaming_word(const StreamingWord &plan) {
  using Saved = StreamingWord::Saved;
  auto word = plan.word;
  const auto place = [&word](unsigned low, unsigned width, unsigned value) {
    const auto mask = ((1U << width) - 1U) << low;
    word = (word & ~mask) | ((value << low) & mask);
  };
  /* Sources first, in x8 and x9 when not resident; the destination, which
   * may also be read, in x10. */
  unsigned fallback = 8;
  for (unsigned index = 0; index < plan.gpr_count; ++index) {
    const auto &gpr = plan.gprs[index];
    if (gpr.destination)
      continue;
    const auto native = read_native_gpr(gpr.guest, gpr.stack_pointer,
                                        fallback++);
    place(gpr.low, 5, native.host);
  }
  std::optional<std::pair<NativeGpr, StreamingWord::Gpr>> destination;
  for (unsigned index = 0; index < plan.gpr_count; ++index) {
    const auto &gpr = plan.gprs[index];
    if (!gpr.destination)
      continue;
    const auto native =
        write_native_gpr(gpr.guest, gpr.stack_pointer, true, 10);
    place(gpr.low, 5, native.host);
    destination.emplace(native, gpr);
  }
  if (plan.slice) {
    /* x12 holds no guest register in a streaming translation. */
    load_gpr(12, plan.slice_guest, false);
    place(plan.slice_low, plan.slice_width, 0);
  }
  if (plan.reads_flags)
    load_flags();
  if (!plan.raises) {
    emitter_.emit(word);
  } else {
    /*
     * XNU resets FPSR to 0x0800009F on every kernel entry while a thread
     * is in streaming mode, so the guest's FPSR stays in CpuState: run the
     * word with FPSR clear and OR what it raised into the guest's. No
     * streaming-mode instruction sets QC, so a QC bit means a kernel entry
     * came between, and the word runs again on its saved inputs.
     */
    const bool predicate = plan.saved == Saved::p;
    const unsigned offset =
        plan.saved_register + (predicate ? predicate_fill_bias : 0U);
    if (plan.saved != Saved::none) {
      emitter_.load_x(16, 15, offsetof(CpuState, sme_registers));
      if (predicate)
        emitter_.emit(addsvl(16, 16, predicate_base_svl));
      emitter_.emit(
          register_fill(false, predicate, 16, offset, plan.saved_register));
    }
    if (plan.reads_flags)
      emitter_.emit(save_flags_x16);
    const auto retry = emitter_.make_label();
    const auto done = emitter_.make_label();
    emitter_.bind(retry);
    emitter_.emit(clear_fpsr);
    emitter_.emit(word);
    emitter_.emit(read_fpsr_x11);
    emitter_.test_zero(done, 11, fpsr_qc_bit, false);
    if (plan.saved != Saved::none) {
      emitter_.load_x(11, 15, offsetof(CpuState, sme_registers));
      if (predicate)
        emitter_.emit(addsvl(11, 11, predicate_base_svl));
      emitter_.emit(
          register_fill(true, predicate, 11, offset, plan.saved_register));
    }
    if (plan.reads_flags)
      emitter_.emit(restore_flags_x16);
    emitter_.branch(retry);
    emitter_.bind(done);
    emitter_.load_w(16, 15, offsetof(CpuState, fpsr));
    emitter_.emit(0x2a0b'0210U); // orr w16, w16, w11
    emitter_.store_w(16, 15, offsetof(CpuState, fpsr));
  }
  if (plan.writes_flags)
    store_flags();
  if (destination)
    commit_native_gpr(destination->first, destination->second.guest,
                      destination->second.stack_pointer);
}

void JitCompiler::emit_movprfx(std::uint32_t instruction, bool predicated) {
  /*
   * MOVPRFX runs as the move it is architecturally, never paired with the
   * guest's next word: nothing the translation puts around that word can
   * then come between a native pair.
   */
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  if (!predicated) {
    emitter_.emit(0x0460'3000U | (source << 16) | (source << 5) |
                  destination); // orr zd.d, zn.d, zn.d
    return;
  }
  const unsigned size = (instruction >> 22) & 3U;
  const unsigned governing = (instruction >> 10) & 7U;
  if ((instruction & 0x0001'0000U) != 0U) {
    emitter_.emit(0x0520'c000U | (size << 22) | (destination << 16) |
                  (governing << 10) | (source << 5) |
                  destination); // sel zd.t, pg, zn.t, zd.t
    return;
  }
  /* Zeroing: the MOVPRFX itself, completed by LSL #0, a prefixed form
   * with the same predicate and element size. */
  const unsigned shift_size = 1U << size;
  emitter_.emit(instruction);
  emitter_.emit(0x0403'8000U | ((shift_size >> 2) << 22) | (governing << 10) |
                ((shift_size & 3U) << 8) | destination);
}

void JitCompiler::emit_memory_span(const detail::SmeMemory &memory,
                                   std::uint32_t instruction,
                                   unsigned destination) {
  /* Registers x SVL x msize / esize: RDSVL by the register count, then a
   * shift by log2(esize / msize). */
  using Shape = detail::SmeMemoryShape;
  unsigned multiple = 1;
  unsigned shift = 0;
  if (memory.shape == Shape::contiguous || memory.shape == Shape::structure) {
    multiple = memory.registers;
    shift = static_cast<unsigned>(std::countr_zero(
        jit::sme_memory_element_bytes(memory, instruction) /
        (memory.msize / 8U)));
  } else if (memory.shape == Shape::fill_p) {
    shift = 3;
  }
  emitter_.emit(rdsvl(destination, multiple));
  if (shift != 0U)
    emitter_.ubfm(destination, destination, shift, 63U); // lsr
}

bool JitCompiler::emit_streaming_memory(const detail::SmeEncoding &row,
                                        std::uint32_t instruction,
                                        std::uint64_t pc) {
  /*
   * The guest address is in x8. When every byte the access can span,
   * whichever elements are active, lies in one guest page that the TLB
   * holds, the guest's own word runs on the host address with its offset
   * zeroed. A crossing, a miss or a misaligned SP base takes the helper,
   * which alone knows which elements are active, so a failed translation
   * goes there too rather than to a fault exit.
   */
  using Shape = detail::SmeMemoryShape;
  const auto &memory = *detail::sme_encoding_memory_of(row);
  /* A prefetch accesses nothing and cannot fault. */
  if (memory.shape == Shape::prefetch)
    return true;
  const auto *operands = detail::sme_encoding_operands_of(row);
  const auto &base_operand = operands[memory.base_operand];
  const detail::SmeOperand *index_operand = nullptr;
  if (memory.index_operand != detail::sme_no_operand)
    index_operand = &operands[memory.index_operand];
  if (!one_field(base_operand, 5) ||
      (index_operand != nullptr && !one_field(*index_operand, 5)))
    return false;
  const bool za = memory.shape == Shape::za_slice;
  /* A ZA slice's index register, W12-W15, runs in W12, as for the
   * register-only forms. */
  const detail::SmeOperand *slice_operand = nullptr;
  for (unsigned index = 0; za && index < row.operand_count; ++index) {
    const auto &operand = operands[index];
    if (operand.kind == detail::SmeOperandKind::gpr &&
        operand.segment_count == 2U && !operand.segments[0].field &&
        operand.segments[1].field &&
        (12U >> operand.segments[1].width) == operand.segments[0].constant)
      slice_operand = &operand;
  }
  if (za && slice_operand == nullptr)
    return false;
  const bool store = memory.direction == detail::SmeMemoryDirection::store;
  const auto access = store ? Access::write : Access::read;
  const unsigned base = detail::sme_operand_number(base_operand, instruction);
  /* LD1R and LD1RQ span a constant few bytes; every other shape a
   * multiple of the SVL, read at run time. */
  const bool constant_span =
      memory.shape == Shape::replicate || memory.shape == Shape::replicate_quad;
  const auto constant_bytes = jit::sme_memory_span(memory, instruction, 0);

  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  load_gpr(8, base, true);
  if (base == 31U)
    emit_misalignment_check(8, 16, slow);
  if (!constant_span)
    emit_memory_span(memory, instruction, 10);
  switch (memory.offset) {
  case detail::SmeAddressOffset::immediate_footprint:
    if (const auto immediate = jit::sme_memory_immediate(memory, instruction);
        immediate != 0) {
      emitter_.load_constant(11, static_cast<std::uint64_t>(immediate));
      emitter_.emit(multiply_add(8, 10, 11, 8));
    }
    break;
  case detail::SmeAddressOffset::register_element: {
    /* A ZA slice's XZR index adds nothing. */
    const unsigned index =
        detail::sme_operand_number(*index_operand, instruction);
    if (index != 31U) {
      const auto native = read_native_gpr(index, false, 9);
      emitter_.add_shifted(
          8, 8, native.host,
          static_cast<unsigned>(std::countr_zero(memory.msize / 8U)));
    }
    break;
  }
  case detail::SmeAddressOffset::immediate_element:
  case detail::SmeAddressOffset::immediate_16: {
    /* At most 63 elements of 8 bytes, or 16 bytes times -8 to 7. */
    const auto offset = static_cast<std::int64_t>(
        jit::sme_memory_offset(memory, instruction, 0, 0));
    if (offset > 0)
      emitter_.add_immediate(8, 8, static_cast<unsigned>(offset));
    else if (offset < 0)
      emitter_.subtract_immediate(8, 8, static_cast<unsigned>(-offset));
    break;
  }
  case detail::SmeAddressOffset::none:
  case detail::SmeAddressOffset::vector_index:
  case detail::SmeAddressOffset::register_byte:
    break;
  }
  emit_untag_data_address(8);
  if (constant_span) {
    emit_translate(8, access, slow, slow, constant_bytes);
  } else {
    /* The span lies below a page, so the page bit of (address & page
     * mask) + span - 1 is the crossing test. */
    emitter_.ubfm(0, 8, 0, page_shift - 1U);
    emitter_.add_shifted(0, 0, 10, 0);
    emitter_.subtract_immediate(0, 0, 1);
    emitter_.test_zero(slow, 0, page_shift, true);
    emit_translate(8, access, slow, slow, 1);
  }
  /* Only loads span a constant; a TLB miss bridge clobbers x10. */
  if (store && track_direct_stores_) {
    emit_memory_span(memory, instruction, 10);
    track_direct_store_register(8, 10, count_);
  }
  auto word = instruction;
  const auto place = [&word](unsigned low, unsigned width, unsigned value) {
    const auto mask = ((1U << width) - 1U) << low;
    word = (word & ~mask) | ((value << low) & mask);
  };
  place(base_operand.segments[0].low, 5, 8);
  for (unsigned index = 0; index < memory.immediate_segment_count; ++index)
    place(memory.immediate[index].low, memory.immediate[index].width, 0);
  if (index_operand != nullptr) {
    /* SVE's scalar-plus-scalar forms have no XZR index (their rows exclude
     * Rm = 31), so they take a zeroed x9; a ZA slice's takes XZR. */
    place(index_operand->segments[0].low, 5, za ? 31U : 9U);
    if (!za)
      emitter_.load_constant(9, 0);
  }
  if (slice_operand != nullptr) {
    /* x12 holds no guest register in a streaming translation. */
    load_gpr(12, detail::sme_operand_number(*slice_operand, instruction),
             false);
    place(slice_operand->segments[1].low, slice_operand->segments[1].width, 0);
  }
  emitter_.emit(word);
  return true;
}

bool JitCompiler::emit_streaming_fp_memory(std::uint32_t instruction,
                                           std::uint64_t pc) {
  /*
   * A SIMD&FP load or store in a streaming translation, on the guest's
   * resident Z registers: when the bytes it moves lie in one guest page
   * the TLB holds, the unsigned-offset (or pair offset) form of the word
   * runs on the host address in x8 and the guest's writeback follows. A
   * crossing, a miss or a misaligned SP base takes the helper, as the
   * vector memory forms do.
   */
  const auto decoded = jit::decode_streaming_fp_memory(instruction);
  if (!decoded || detail::classify_fp_simd_streaming(instruction) !=
                      detail::FpSimdStreaming::fp)
    return false;
  const auto &memory = *decoded;
  using Mode = jit::FpMemoryMode;
  const auto access = memory.load ? Access::read : Access::write;
  const std::size_t bytes = std::size_t{memory.bytes} * memory.registers;
  if (!memory.load)
    mark_direct_memory_write();
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sme_slow);
  if (memory.mode == Mode::literal) {
    emitter_.load_constant(
        8, pc + static_cast<std::uint64_t>(memory.immediate));
  } else {
    load_gpr(8, memory.base, true);
    if (memory.base == 31U)
      emit_misalignment_check(8, 16, slow);
    if (memory.mode == Mode::register_offset) {
      /* ADD x8, x8, Rm, <extend> #shift; XZR reads as zero. */
      const auto index = read_native_gpr(memory.index, false, 9);
      emitter_.emit(0x8b20'0000U | (index.host << 16) |
                    (unsigned{memory.option} << 13) |
                    (unsigned{memory.shift} << 10) | (8U << 5) | 8U);
    } else if (memory.mode != Mode::post_index) {
      /* LDR Q's unsigned offset reaches 65520: two 12-bit halves. */
      const auto magnitude = static_cast<unsigned>(
          memory.immediate < 0 ? -memory.immediate : memory.immediate);
      for (const bool high : {false, true}) {
        const unsigned part = high ? magnitude >> 12 : magnitude & 0xfffU;
        if (part == 0U)
          continue;
        if (memory.immediate < 0)
          emitter_.subtract_immediate(8, 8, part, high);
        else
          emitter_.add_immediate(8, 8, part, high);
      }
    }
    emit_untag_data_address(8);
  }
  emit_translate(8, access, slow, slow, bytes);
  if (!memory.load)
    track_direct_store(8, bytes);
  const unsigned rt = memory.transfer;
  std::uint32_t word;
  if (memory.registers == 2U) {
    const unsigned opc = memory.bytes == 4U ? 0U : memory.bytes == 8U ? 1U : 2U;
    word = (opc << 30) |
           (memory.nontemporal ? 0x2c00'0000U : 0x2d00'0000U) |
           (memory.load ? 0x0040'0000U : 0U) |
           (unsigned{memory.transfer2} << 10) | (8U << 5) | rt;
  } else {
    const unsigned size =
        memory.bytes == 16U ? 0U
                            : static_cast<unsigned>(std::countr_zero(
                                  unsigned{memory.bytes}));
    const unsigned opc = (memory.bytes == 16U ? 2U : 0U) | (memory.load ? 1U : 0U);
    word = (size << 30) | 0x3d00'0000U | (opc << 22) | (8U << 5) | rt;
  }
  emitter_.emit(word);
  if ((memory.mode == Mode::pre_index || memory.mode == Mode::post_index) &&
      memory.immediate != 0) {
    const auto base = write_native_gpr(memory.base, true, true, 9);
    if (memory.immediate > 0)
      emitter_.add_immediate(base.host, base.host,
                             static_cast<unsigned>(memory.immediate));
    else
      emitter_.subtract_immediate(base.host, base.host,
                                  static_cast<unsigned>(-memory.immediate));
    commit_native_gpr(base, memory.base, true);
  }
  return true;
}

void JitCompiler::emit_terminal_helper(const void *helper,
                                       std::uint32_t instruction,
                                       std::uint64_t pc, std::size_t count) {
  store_pc(pc);
  spill_gprs_for_terminal_call();
  emitter_.move_x(0, 15);
  emitter_.move_x(1, 14);
  emitter_.load_constant(2, instruction);
  emitter_.call(helper);
  emitter_.return_dynamic(count);
}

} // namespace uml::cpu::arm64
