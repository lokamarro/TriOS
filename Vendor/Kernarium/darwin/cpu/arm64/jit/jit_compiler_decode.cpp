// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/detail/bfloat_dot.hpp"
#include "cpu/arm64/detail/bfloat_matrix.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/floating_compare.hpp"
#include "cpu/arm64/detail/floating_complex.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/floating_fused.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"
#include "cpu/arm64/detail/floating_integer_conversion.hpp"
#include "cpu/arm64/detail/floating_reciprocal.hpp"
#include "cpu/arm64/detail/floating_round.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"
#include "cpu/arm64/detail/simd_floating_classes.hpp"
#include "cpu/arm64/detail/sm0_vector_length.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"

namespace uml::cpu::arm64 {

namespace {

/* An FP&SIMD data-processing word, or a load or store of SIMD&FP
 * registers (load/store class with V set). */
[[nodiscard]] constexpr bool fp_simd_word(std::uint32_t instruction) noexcept {
  return floating_or_vector_primary_class(instruction) ||
         (instruction & 0x0e00'0000U) == 0x0c00'0000U;
}

static_assert(fp_simd_word(0x4ea1'1c20U));  // mov v0.16b, v1.16b
static_assert(fp_simd_word(0x1e60'4020U));  // fmov d0, d1
static_assert(fp_simd_word(0x3dc0'0020U));  // ldr q0, [x1]
static_assert(fp_simd_word(0xad40'0440U));  // ldp q0, q1, [x2]
static_assert(fp_simd_word(0x4c40'7020U));  // ld1 {v0.16b}, [x1]
static_assert(!fp_simd_word(0xf940'0020U)); // ldr x0, [x1]
static_assert(!fp_simd_word(0xd53b'4400U)); // mrs x0, fpcr

} // namespace

bool JitCompiler::emit_common_control_flow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t resulting_count) {
    if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
      const auto target =
          pc + sign_extend(instruction & 0x03ff'ffffU, 26) * 4;
      if ((instruction & 0x8000'0000U) != 0) {
        emitter_.load_literal_constant(8, pc + 4);
        store_gpr(8, 30, false);
      }
      emit_direct_branch(target, resulting_count);
      return true;
    }
    if ((instruction & 0xff00'0010U) == 0x5400'0000U) {
      const auto target =
          pc + sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
      emit_conditional_pc(instruction & 15U, target, pc + 4,
                          resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
      emit_compare_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
      emit_test_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
      emit_register_branch(instruction, pc, resulting_count);
      return true;
    }
    return false;
  }

JitCompiler::Action JitCompiler::emit_instruction(std::uint32_t instruction, std::uint64_t pc) {
    const auto resulting_count = count_ + 1;
    const auto reject = [&] {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    };
    if ((instruction & 0xffff'0000U) == 0U)
      return reject();
    /*
     * The FP/SIMD class decoders below match only the bits that separate
     * their own allocations, and many copy the guest word into host code.
     * A word in an unallocated region of the class table must stop here,
     * like any other word without a translation: reaching the nearest
     * decoder would execute a neighbour or raise a host SIGILL inside the
     * generated block.
     */
    if (arm64::detail::unallocated_simd_floating_class(instruction))
      return Action::unsupported;
    /*
     * A streaming translation runs the FP&SIMD data-processing words the
     * profile marks FP-legal natively, on the guest's resident vectors
     * (jit::sme_support), and the SIMD&FP loads and stores streaming mode
     * keeps. AdvSIMD is illegal in streaming mode. MRS and MSR of FPCR and
     * FPSR stay legal (emit_system_register), and so do FEAT_MOPS's CPY
     * words, whose bit 26 only looks like a SIMD&FP load or store's.
     */
    if (streaming_translation() && fp_simd_word(instruction) &&
        !arm64::detail::decode_mops(instruction).recognized) {
      if ((instruction & 0x0a00'0000U) == 0x0800'0000U)
        return emit_streaming_fp_memory(instruction, pc)
                   ? Action::keep_going
                   : Action::unsupported;
      const auto plan = plan_streaming_fp_simd(instruction);
      if (!plan)
        return Action::unsupported;
      emit_streaming_word(*plan);
      return Action::keep_going;
    }
    /* Every SVE and SME word goes through the profile's table first.
     * Without SME it stays on the path that makes it the illegal exit,
     * unchanged. */
    if (arm64::detail::sme_encoding_space(instruction)) {
      const auto &host = host_capabilities();
      /*
       * The SM = 0 vector-length family
       * (detail/sm0_vector_length.hpp): an SME virtual CPU translates a
       * vector-length word as the base word of the same effect and a spill
       * word as the memory access it is.
       */
      if (!streaming_translation() &&
          arm64::detail::implements_sm0_vector_length(host)) {
        if (const auto equivalent =
                arm64::detail::sm0_vector_length_equivalent(instruction))
          return emit_instruction(*equivalent, pc);
        if (const auto spill = arm64::detail::sm0_spill_access(instruction)) {
          emit_sm0_spill(*spill, instruction, pc);
          return Action::keep_going;
        }
      }
      if (implements_sme(host))
        return emit_sme_instruction(instruction, pc);
    }
    if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
      store_pc(pc + 4);
      emitter_.return_constant(Event::system_call, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'001fU) == 0xd420'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::breakpoint, resulting_count);
      return Action::terminate;
    }
    /* BC.cond needs FEAT_HBC, which the virtual CPU does not implement. */
    if ((instruction & 0xff00'0010U) == 0x5400'0010U)
      return reject();
    if (emit_common_control_flow(instruction, pc, resulting_count))
      return Action::terminate;
    if ((instruction & 0xffe0'001fU) == 0xd400'0002U ||
        (instruction & 0xffe0'001fU) == 0xd400'0003U ||
        (instruction & 0xffe0'001fU) == 0xd440'0000U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0001U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0002U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0003U ||
        instruction == 0xd6bf'03e0U || instruction == 0xd69f'03e0U ||
        instruction == 0xd69f'0bffU || instruction == 0xd69f'0fffU)
      return reject();
    /*
     * The virtual CPU implements no FEAT_GCS: GCSSTR and GCSSTTR are
     * unallocated. The GCS SYS and SYSL forms take the generic SYS path.
     */
    if ((instruction & 0xffff'ec00U) == 0xd91f'0c00U)
      return reject();
    /*
     * Ordered-memory allocations overlap broader ordinary, structure, and
     * atomic load/store classes. Resolve the complete allocations here so
     * reserved encodings are rejected before any address is observed and so
     * valid native-JIT cases never fall through to gadget translation.
     *
     * The checked memory bridge is a cold-path execution boundary: register,
     * width, ordering, and addressing-mode decode remains at translation
     * time, while the bridge supplies the software-MMU address override.
     */
    const auto ordered_memory = [&](bool store) {
      if (store)
        mark_memory_write();
      emit_terminal_helper(
          reinterpret_cast<const void *>(&uml_arm64_jit_execute_memory),
          instruction, pc, resulting_count);
      return Action::terminate;
    };

    /*
     * The virtual CPU implements neither FEAT_LRCPC3 nor FEAT_LSCP: LDAP1,
     * STL1, LDAPUR and STLUR to SIMD&FP registers, LDAPR and STLR with
     * writeback, LDIAPP, STILP, and the LDAP, LDAPP, and STLP pairs are
     * undefined.
     */
    if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U ||
        (instruction & 0x3f20'0c00U) == 0x1d00'0800U ||
        (instruction & 0x3f20'0c00U) == 0x1900'0800U)
      return reject();
    if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U) {
      if (!host_capabilities().has(ArmFeature::lrcpc))
        return Action::unsupported;
      emit_native_ordered_load(instruction, pc,
                               ColdPath::Action::memory_slow);
      return Action::keep_going;
    }
    if ((instruction & 0x3f20'0c00U) == 0x1900'0000U) {
      const unsigned size = instruction >> 30;
      const unsigned operation = (instruction >> 22) & 3U;
      if ((operation == 2U && size == 3U) ||
          (operation == 3U && size >= 2U))
        return reject();
      if (!host_capabilities().has(ArmFeature::lrcpc2))
        return Action::unsupported;
      return ordered_memory(operation == 0U);
    }
    /*
     * The virtual CPU implements no FEAT_MTE. Resolve the tag-immediate
     * (ADDG, SUBG) and tag-memory allocations as undefined before the
     * ordinary immediate-arithmetic and load/store decoders see them. The
     * FEAT_CSSC min/max immediates share the tag-immediate class's major
     * encoding and keep their own decoder.
     */
    if (((instruction & 0x1f80'0000U) == 0x1180'0000U &&
         (instruction & 0x7ff0'0000U) != 0x11c0'0000U) ||
        (instruction & 0xff20'0000U) == 0xd920'0000U)
      return reject();
    if (const auto action = emit_sme_system(instruction, pc))
      return *action;
    if (emit_system_register(instruction)) {
      /*
       * A register-sourced FPCR write may enable precise guest exception
       * reporting.  Keep ordinary control-only writes inside the native
       * block, but leave immediately when the newly stored value actually
       * contains an enable so the next instruction passes through the cold
       * gate.  The host write itself has already masked guest-only enables.
       */
      if ((instruction & 0xffff'ffe0U) == 0xd51b'4400U &&
          (instruction & 31U) != 31U) {
        const auto no_exception_enable = emitter_.make_label();
        emitter_.load_w(8, 15, offsetof(CpuState, fpcr));
        emitter_.load_constant(
            9, arm64::detail::fpcr_exception_enable_mask);
        emitter_.emit_internal_flag_clobber(
            0x6a00'001fU | (9U << 16) | (8U << 5));
        emitter_.branch_condition(no_exception_enable, 0); // EQ
        store_pc(pc + sizeof(std::uint32_t));
        emitter_.return_constant(Event::continued, resulting_count);
        emitter_.bind(no_exception_enable);
      }
      return Action::keep_going;
    }
    if ((instruction & 0xffd0'0000U) == 0xd510'0000U ||
        (instruction & 0xffd0'0000U) == 0xd550'0000U ||
        (instruction & 0xfff8'0000U) == 0xd548'0000U)
      return reject();
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7420U) {
      emit_cache_zero(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7a20U ||
        (instruction & 0xffff'ffe0U) == 0xd50b'7b20U ||
        (instruction & 0xffff'ffe0U) == 0xd50b'7e20U)
      return Action::keep_going;
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7520U) {
      emit_code_synchronize(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfff8'0000U) == 0xd508'0000U ||
        (instruction & 0xfff8'0000U) == 0xd528'0000U)
      return reject();
    if ((instruction & 0xfff8'f01fU) == 0xd500'401fU) {
      const unsigned op1 = (instruction >> 16) & 7U;
      const unsigned immediate = (instruction >> 8) & 15U;
      const unsigned op2 = (instruction >> 5) & 7U;
      if (op1 == 0U && immediate == 0U && op2 <= 2U) {
        if (emit_flag_manipulation(instruction))
          return Action::keep_going;
        return Action::unsupported;
      } else {
        if (op1 != 3U)
          return reject();

        const auto store_boolean = [&](std::size_t offset) {
          emitter_.load_constant(8, immediate & 1U);
          emitter_.store_b(8, 15, offset);
        };
        /*
         * The virtual CPU implements neither FEAT_SSBS (op2 1) nor FEAT_SME
         * (op2 3, SMSTART and SMSTOP), and MSR TCO (op2 4) needs FEAT_MTE:
         * those fields are unallocated.
         */
        switch (op2) {
        case 2:
          if (!translation_supports(ArmFeature::dit))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_dit));
          return Action::keep_going;
        case 6:
        case 7: {
          const auto denied = emitter_.make_label();
          const auto complete = emitter_.make_label();
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif_access));
          emitter_.compare_zero(denied, 8, false, false);
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.load_constant(9, immediate);
          emitter_.emit((op2 == 6 ? 0x2a00'0000U : 0x0a20'0000U) |
                        (9U << 16) | (8U << 5) | 8U);
          emitter_.store_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.branch(complete);
          emitter_.bind(denied);
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction,
                                   resulting_count);
          emitter_.bind(complete);
          return Action::keep_going;
        }
        default:
          return reject();
        }
      }
    }
    if ((instruction & 0xffe0'7c10U) == 0xba00'0400U ||
        (instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
      if (emit_flag_manipulation(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
        (instruction & 0xffff'0800U) == 0x3a00'0800U)
      return reject();
    if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30dfU ||
        instruction == 0xd503'30ffU) {
      const auto lowered = lower_memory_barrier(
          instruction, host_capabilities().has(ArmFeature::sb));
      for (std::size_t index = 0; index < lowered.count; ++index)
        emitter_.emit(lowered.words[index]);
      if ((instruction & 0xffff'f0ffU) == 0xd503'30dfU)
        emit_code_change_check(instruction, pc);
      return Action::keep_going;
    }
    /* SB with a nonzero CRm is reserved, and DSB nXS needs FEAT_XS, which
     * the virtual CPU does not implement. */
    if ((instruction & 0xffff'f0ffU) == 0xd503'30ffU ||
        (instruction & 0xffff'f3ffU) == 0xd503'323fU)
      return reject();
    if ((instruction & 0xffff'f0ffU) == 0xd503'305fU) {
      emitter_.emit(instruction);
      emitter_.load_constant(8, ~std::uint64_t{0});
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_address));
      emitter_.load_constant(8, 0);
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_key));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value2));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_version));
      emitter_.store_w(8, 15, offsetof(CpuState, exclusive_size));
      return Action::keep_going;
    }
    if (emit_primary_hint(instruction) ||
        (instruction & 0xffd8'f000U) == 0xd500'2000U)
      return Action::keep_going;
    /*
     * The guest profile does not expose the enhanced return-address
     * authentication extension. Resolve its complete fixed, immediate, and
     * register allocations here so valid unavailable forms and reserved
     * neighbors terminate deterministically instead of entering an
     * unsupported native block.
     */
    if ((instruction & 0xffff'c000U) == 0xdac1'8000U ||
        (instruction & 0xffc0'0000U) == 0xf380'0000U ||
        (instruction & 0xffc0'0000U) == 0x5500'0000U ||
        ((instruction & 0xffff'fbe0U) == 0xd65f'0be0U &&
         (instruction & 31U) != 31U))
      return reject();
    /* The authenticated branches and returns need FEAT_PAuth, which the
     * virtual CPU does not implement. */
    if ((instruction & 0xfedf'f800U) == 0xd61f'0800U ||
        (instruction & 0xffff'fbffU) == 0xd65f'0bffU)
      return reject();
    /* WFET and WFIT need FEAT_WFxT, which the virtual CPU does not
     * implement. */
    if ((instruction & 0xffff'ffc0U) == 0xd503'1000U)
      return reject();
    /* The virtual CPU implements no FEAT_CMPBR: the compare-and-branch class
     * is unallocated. */
    if ((instruction & 0x7e00'0000U) == 0x7400'0000U)
      return reject();
    if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
      emit_pc_relative(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
      if (!valid_move_wide(instruction))
        return reject();
      emit_move_wide(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
      emit_integer_min_max(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
      emit_add_sub_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f20'0000U) == 0x0b00'0000U) {
      if (!valid_shifted_register(instruction, true))
        return reject();
      emit_native_binary(instruction, false, (instruction & 0x2000'0000U) != 0);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
      if (!valid_extended_register(instruction))
        return reject();
      emit_add_sub_extended(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
      emit_add_sub_carry(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
      if (!valid_logical_immediate(instruction))
        return reject();
      emit_logical_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
      if (!valid_bitfield(instruction))
        return reject();
      emit_bitfield(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      if (((instruction >> 29) & 3U) != 0U ||
          (instruction & 0x0020'0000U) != 0U ||
          ((instruction >> 22) & 1U) != static_cast<unsigned>(wide) ||
          (!wide && ((instruction >> 10) & 63U) >= 32U))
        return reject();
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
      if (!valid_shifted_register(instruction, false))
        return reject();
      emit_logical_shifted(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
      if ((instruction & 0x0000'0800U) != 0)
        return reject();
      emit_conditional_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
      if ((instruction & 0x0000'0410U) != 0)
        return reject();
      emit_conditional_compare(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
      if ((instruction & 0xffe0'8000U) == 0x9b60'0000U ||
          (instruction & 0xffe0'8000U) == 0x9b60'8000U)
        return Action::unsupported;
      if (!valid_multiply_add(instruction))
        return reject();
      emit_multiply_add(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
      const unsigned size = (instruction >> 10) & 3U;
      if (((instruction >> 31) != 0) != (size == 3U))
        return reject();
      if (emit_crc32(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0x7fe0'fc00U) == 0x1ac0'2000U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2400U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2c00U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0c00U) {
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
      emit_integer_min_max(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U)
      return reject();
    /* PAC*, AUT*, XPACI, XPACD and PACGA need FEAT_PAuth, which the
     * virtual CPU does not implement. */
    if ((instruction & 0xffff'c000U) == 0xdac1'0000U ||
        (instruction & 0xffff'f800U) == 0xdac1'4000U ||
        (instruction & 0xffe0'fc00U) == 0x9ac0'3000U)
      return reject();
    if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      const bool s = (instruction & 0x2000'0000U) != 0;
      const unsigned opcode = (instruction >> 10) & 63U;
      bool allocated = false;
      if (opcode < 32U) {
        if (s) {
          allocated = wide && opcode == 0U;
        } else if (!wide) {
          allocated = opcode == 2U || opcode == 3U ||
                      (opcode >= 8U && opcode <= 11U) ||
                      (opcode >= 16U && opcode <= 18U) ||
                      (opcode >= 20U && opcode <= 22U) ||
                      (opcode >= 24U && opcode <= 27U);
        } else {
          allocated = opcode == 0U || opcode == 2U || opcode == 3U ||
                      opcode == 4U || opcode == 5U ||
                      (opcode >= 8U && opcode <= 12U) ||
                      opcode == 19U || opcode == 23U ||
                      (opcode >= 24U && opcode <= 27U);
        }
      }
      if (allocated)
        return Action::unsupported;
      return reject();
    }
    if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
      if (!valid_bit_reverse_count(instruction))
        return reject();
      emit_bit_reverse_count(instruction);
      return Action::keep_going;
    }
    /*
     * Decode the exact conversion allocations before the broader SIMD/FP
     * classes below. Several conversion opcodes deliberately reuse fields
     * that those legacy recognizers inspect, including the integer-reduction
     * class.
     */
    if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U) {
      if (!emit_floating_javascript_convert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_to_integer =
            arm64::detail::decode_floating_to_integer(instruction);
        floating_to_integer.recognized) {
      if (!floating_to_integer.valid)
        return reject();
      if (!emit_floating_to_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /*
     * The virtual CPU implements no FEAT_FP8: its conversions, dot products,
     * multiply-adds, and matrix multiplies are undefined.
     */
    if ((instruction & 0xbf3f'fc00U) == 0x2e21'7800U ||
        (instruction & 0xbfa0'fc00U) == 0x0e00'f400U ||
        (instruction & 0xbf80'f400U) == 0x0f00'0000U ||
        (instruction & 0xbfa0'fc00U) == 0x0e00'fc00U ||
        (instruction & 0xbfc0'f400U) == 0x0fc0'0000U ||
        (instruction & 0xbfe0'fc00U) == 0x0ec0'fc00U ||
        (instruction & 0xbf80'f400U) == 0x2f00'8000U ||
        (instruction & 0xbfa0'fc00U) == 0x0e00'c400U ||
        (instruction & 0xff60'fc00U) == 0x6e00'ec00U)
      return reject();
    if (const auto complex =
            arm64::detail::decode_floating_complex(instruction);
        complex.recognized) {
      if (!complex.valid)
        return reject();
      if (!emit_simd_floating_complex(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const unsigned simd_integer_reduce_opcode =
        (instruction >> 12) & 31U;
    const bool simd_integer_reduce =
        (instruction & 0x9f3e'0c00U) == 0x0e30'0800U &&
        (simd_integer_reduce_opcode == 3U ||
         simd_integer_reduce_opcode == 10U ||
         simd_integer_reduce_opcode == 26U ||
         simd_integer_reduce_opcode == 27U);
    if (simd_integer_reduce) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      if (size > 2U || (!q && size == 2U) ||
          (simd_integer_reduce_opcode == 27U && unsigned_group))
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_integer_reduce(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0xff20'fc00U) == 0x1e20'2000U) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool compare_zero = (instruction & 8U) != 0U;
      if (type == 2U || (instruction & 7U) != 0U ||
          (compare_zero && ((instruction >> 16) & 31U) != 0U))
        return reject();
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0400U) {
      if (((instruction >> 22) & 3U) == 2U)
        return reject();
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U) {
      if (((instruction >> 22) & 3U) == 2U)
        return reject();
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
      if (((instruction >> 22) & 3U) == 2U)
        return reject();
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff3f'fc00U) == 0x1e20'4000U) {
      if (((instruction >> 22) & 3U) == 2U)
        return reject();
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, false);
      return Action::keep_going;
    }
    if (const auto floating_compare =
            arm64::detail::decode_simd_floating_compare(instruction);
        floating_compare.recognized) {
      if (!floating_compare.valid)
        return reject();
      if (!emit_simd_floating_compare(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool scalar_precision_allocation =
        (instruction & 0xff20'7c00U) == 0x1e20'4000U &&
        ((instruction >> 15) & 0x3fU) >= 4U &&
        ((instruction >> 15) & 0x3fU) <= 7U;
    if (scalar_precision_allocation) {
      const unsigned source_type = (instruction >> 22) & 3U;
      const unsigned destination_type = (instruction >> 15) & 3U;
      /* The source=01/destination=10 cell belongs to the separate
       * reduced-precision conversion allocation. All other invalid type
       * pairs are reserved scalar FCVT encodings and must terminate as an
       * architectural illegal instruction, even though no native operation
       * can be emitted for them. */
      const bool separate_allocation =
          source_type == 1U && destination_type == 2U;
      if (!separate_allocation &&
          (source_type == 2U || destination_type == 2U ||
           source_type == destination_type))
        return reject();
      if (!separate_allocation) {
        if (!emit_floating_precision_convert(instruction))
          return Action::unsupported;
        return Action::keep_going;
      }
    }
    if (emit_floating_precision_convert(instruction))
      return Action::keep_going;
    if (const auto floating_round =
            arm64::detail::decode_floating_round(instruction);
        floating_round.recognized) {
      if (!floating_round.valid)
        return reject();
      if (!emit_floating_round(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto scalar_sign_operation = instruction & 0xff3f'fc00U;
    const auto half_sign_operation = instruction & 0xbfff'fc00U;
    const auto vector_sign_operation = instruction & 0xbf3f'fc00U;
    const bool scalar_sign =
        scalar_sign_operation == 0x1e20'c000U ||
        scalar_sign_operation == 0x1e21'4000U;
    const bool half_sign =
        half_sign_operation == 0x0ef8'f800U ||
        half_sign_operation == 0x2ef8'f800U;
    const bool vector_sign =
        vector_sign_operation == 0x0e20'f800U ||
        vector_sign_operation == 0x2e20'f800U;
    if (scalar_sign || half_sign || vector_sign) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const bool valid =
          scalar_sign ? type != 2U
                      : half_sign ||
                            ((instruction & 0x0080'0000U) != 0 &&
                             ((instruction & 0x0040'0000U) == 0 || q));
      if (!valid)
        return reject();
      if (!emit_floating_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_floating_unary(instruction))
      return Action::keep_going;
    if (const auto floating_horizontal =
            arm64::detail::decode_floating_horizontal(instruction);
        floating_horizontal.recognized) {
      if (!floating_horizontal.valid)
        return reject();
      if (!emit_floating_horizontal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_reciprocal =
            arm64::detail::decode_floating_reciprocal(instruction);
        floating_reciprocal.recognized) {
      if (!floating_reciprocal.valid)
        return reject();
      if (!emit_floating_reciprocal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /* FSCALE belongs to FEAT_FP8, which the virtual CPU does not implement. */
    if ((instruction & 0xbfe0'fc00U) == 0x2ec0'3c00U ||
        (instruction & 0xbfa0'fc00U) == 0x2ea0'fc00U)
      return reject();
    if (const auto integer_to_floating =
            arm64::detail::decode_integer_to_floating(instruction);
        integer_to_floating.recognized) {
      if (!integer_to_floating.valid)
        return reject();
      if (!emit_integer_to_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /*
     * The virtual CPU implements no FEAT_F16F32DOT: FDOT is undefined. Bit
     * 28 set moves its fields into the scalar classes; the fused decoder
     * owns the words it recognizes there, and the rest are reserved.
     */
    if (((instruction & 0xafe0'fc00U) == 0x0e80'fc00U ||
         (instruction & 0xafc0'f400U) == 0x0f40'9000U) &&
        ((instruction & 0x1000'0000U) == 0U ||
         !arm64::detail::decode_fused_floating_arithmetic(instruction)
              .recognized))
      return reject();
    if (arm64::detail::decode_half_fused_long(instruction).recognized) {
      if (!emit_half_fused_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /* The virtual CPU implements neither FEAT_F16MM nor FEAT_F16F32MM. */
    if ((instruction & 0x9fe0'fc00U) == 0x0ec0'ec00U ||
        (instruction & 0xbfe0'fc00U) == 0x0e40'ec00U)
      return reject();
    if (arm64::detail::reserved_half_fused_long(instruction))
      return reject();
    if (const auto fused_floating =
            arm64::detail::decode_fused_floating_arithmetic(instruction);
        fused_floating.recognized) {
      if (!fused_floating.valid)
        return reject();
      if (!emit_fused_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto basic_floating =
            arm64::detail::decode_basic_floating_arithmetic(instruction);
        basic_floating.recognized) {
      if (!basic_floating.valid)
        return reject();
      if (!emit_basic_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_dot(instruction).recognized) {
      if (!emit_bfloat_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_matrix_multiply(instruction)) {
      if (!emit_bfloat_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_multiply_long(instruction)
            .recognized) {
      if (!emit_bfloat_multiply_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /*
     * Resolve the unallocated cells adjacent to the reduced-precision
     * vector operations before the broader SIMD classes can treat them as
     * unsupported. Q, register fields, and the allocated arrangements have
     * already been consumed by the exact decoders above.
     */
    if ((instruction & 0xbf20'fc00U) == 0x2e00'fc00U) {
      const unsigned size = (instruction >> 22) & 3U;
      if (size == 0U || size == 2U)
        return reject();
    }
    if ((instruction & 0xbf20'fc00U) == 0x2e00'ec00U) {
      const bool q = (instruction & 0x4000'0000U) != 0U;
      const unsigned size = (instruction >> 22) & 3U;
      if ((!q && size == 1U) || (q && size == 3U))
        return reject();
    }
    if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned offset = (instruction >> 11) & 15U;
      if (!q && offset >= 8U)
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_extract(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /* LUTI2 and LUTI4 need FEAT_LUT, which the virtual CPU does not
     * implement. */
    if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
        (instruction & 0x00c0'0000U) != 0U)
      return reject();
    if (emit_simd_table_lookup(instruction))
      return Action::keep_going;
    const auto simd_permute_operation = instruction & 0xbf20'fc00U;
    const bool simd_permute = simd_permute_operation == 0x0e00'1800U ||
                              simd_permute_operation == 0x0e00'2800U ||
                              simd_permute_operation == 0x0e00'3800U ||
                              simd_permute_operation == 0x0e00'5800U ||
                              simd_permute_operation == 0x0e00'6800U ||
                              simd_permute_operation == 0x0e00'7800U;
    if (simd_permute && (instruction & 0x4000'0000U) == 0 &&
        ((instruction >> 22) & 3U) == 3U)
      return reject();
    if (emit_simd_permute(instruction))
      return Action::keep_going;
    const bool simd_aes =
        (instruction & 0xffff'8c00U) == 0x4e28'0800U;
    if (simd_aes && ((instruction >> 12) & 7U) < 4U)
      return reject();
    if (emit_simd_aes(instruction))
      return Action::keep_going;
    const bool simd_pmull =
        (instruction & 0xbf20'fc00U) == 0x0e20'e000U;
    const unsigned simd_pmull_size = (instruction >> 22) & 3U;
    if (simd_pmull && simd_pmull_size != 0U && simd_pmull_size != 3U)
      return reject();
    if (emit_simd_pmull(instruction))
      return Action::keep_going;
    const bool simd_sha12_three =
        (instruction & 0xffe0'8c00U) == 0x5e00'0000U;
    const bool simd_sha12_two =
        (instruction & 0xffff'cc00U) == 0x5e28'0800U;
    if ((simd_sha12_three && ((instruction >> 12) & 7U) == 7U) ||
        (simd_sha12_two && ((instruction >> 12) & 3U) == 3U))
      return reject();
    if (emit_simd_sha1_sha2(instruction))
      return Action::keep_going;
    if (emit_simd_sha512(instruction))
      return Action::keep_going;
    /*
     * SM3PARTW1, SM3PARTW2, SM4EKEY, SM3SS1, SM3TT*, and SM4E need FEAT_SM3
     * and FEAT_SM4, which the virtual CPU does not implement; the other
     * cells are reserved.
     */
    const bool reserved_simd_logic =
        (instruction & 0xffe0'f000U) == 0xce60'c000U ||
        (instruction & 0xffe0'8000U) == 0xce40'0000U ||
        (instruction & 0xffe0'c000U) == 0xce40'8000U ||
        (instruction & 0xffe0'8000U) == 0xce60'0000U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8400U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8c00U;
    if (reserved_simd_logic)
      return reject();
    if (emit_simd_sha3(instruction))
      return Action::keep_going;
    const auto simd_high_narrow_class = instruction & 0x9f20'fc00U;
    const bool simd_high_narrow =
        simd_high_narrow_class == 0x0e20'4000U ||
        simd_high_narrow_class == 0x0e20'6000U;
    if (simd_high_narrow && ((instruction >> 22) & 3U) == 3U)
      return reject();
    if (emit_simd_arithmetic_narrow(instruction))
      return Action::keep_going;
    const bool simd_widening_vector =
        (instruction & 0x9f20'0c00U) == 0x0e20'0000U;
    const bool simd_widening_scalar =
        (instruction & 0xdf20'0c00U) == 0x5e20'0000U;
    const unsigned simd_widening_operation = (instruction >> 12) & 15U;
    const bool simd_widening =
        (simd_widening_vector || simd_widening_scalar) &&
        simd_widening_operation != 4U &&
        simd_widening_operation != 6U &&
        simd_widening_operation != 14U;
    if (simd_widening) {
      const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool saturating = simd_widening_operation == 9U ||
                              simd_widening_operation == 11U ||
                              simd_widening_operation == 13U;
      const bool valid =
          size < 3U &&
          (simd_widening_scalar
               ? saturating && !unsigned_values && size != 0U
               : saturating
                     ? !unsigned_values && size != 0U
                     : simd_widening_operation <= 3U ||
                           simd_widening_operation == 5U ||
                           simd_widening_operation == 7U ||
                           simd_widening_operation == 8U ||
                           simd_widening_operation == 10U ||
                           simd_widening_operation == 12U);
      if (!valid)
        return reject();
    }
    if (emit_simd_widening(instruction))
      return Action::keep_going;
    if (emit_simd_narrow_move(instruction))
      return Action::keep_going;
    const bool simd_two_register_vector =
        (instruction & 0x9f3e'0c00U) == 0x0e20'0800U;
    const bool simd_two_register_scalar =
        (instruction & 0xdf3e'0c00U) == 0x5e20'0800U;
    const unsigned simd_two_register_opcode = (instruction >> 12) & 31U;
    const bool simd_two_register_integer =
        (simd_two_register_vector || simd_two_register_scalar) &&
        (simd_two_register_opcode == 2U ||
         simd_two_register_opcode == 3U ||
         simd_two_register_opcode == 4U ||
         simd_two_register_opcode == 6U ||
         simd_two_register_opcode == 7U ||
         simd_two_register_opcode == 11U ||
         simd_two_register_opcode == 18U ||
         simd_two_register_opcode == 19U ||
         simd_two_register_opcode == 20U);
    if (simd_two_register_integer) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      bool valid = false;
      switch (simd_two_register_opcode) {
      case 2:
      case 4:
      case 6:
        valid = simd_two_register_vector && size < 3U;
        break;
      case 3:
      case 7:
        valid = simd_two_register_scalar || q || size < 3U;
        break;
      case 11:
        valid =
            simd_two_register_scalar ? size == 3U : q || size < 3U;
        break;
      case 18:
        valid = size < 3U &&
                (unsigned_group || simd_two_register_vector);
        break;
      case 19:
        valid = size < 3U && unsigned_group &&
                simd_two_register_vector;
        break;
      case 20:
        valid = size < 3U;
        break;
      }
      if (!valid)
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_two_register_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (simd_two_register_scalar &&
        (simd_two_register_opcode == 0U ||
         simd_two_register_opcode == 1U ||
         simd_two_register_opcode == 5U)) {
      /* Reverse and byte-unary operations have no scalar allocation. */
      return reject();
    }
    if ((instruction & 0x9fbf'fc00U) == 0x0ea1'c800U) {
      if ((instruction & 0x0040'0000U) != 0)
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_unsigned_estimate(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_fmulx(instruction)) {
      if (!valid_simd_fmulx(instruction))
        return reject();
      if (!emit_simd_fmulx(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_dot(instruction)) {
      if (!valid_simd_dot(instruction))
        return reject();
      if (!emit_simd_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_matrix(instruction)) {
      if (!valid_simd_matrix(instruction))
        return reject();
      if (!emit_simd_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_floating_indexed(instruction)) {
      if (!valid_simd_floating_indexed(instruction))
        return reject();
      if (!emit_simd_floating_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_integer_indexed(instruction)) {
      if (!valid_simd_integer_indexed(instruction))
        return reject();
      if (!emit_simd_integer_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_rdm(instruction)) {
      if (!valid_simd_rdm(instruction))
        return reject();
      if (!emit_simd_rdm(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
      if (!emit_simd_bitwise_register(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_three_same_vector =
        (instruction & 0x9f20'0400U) == 0x0e20'0400U;
    const bool simd_three_same_scalar =
        (instruction & 0xdf20'0400U) == 0x5e20'0400U;
    const unsigned simd_three_same_opcode = (instruction >> 11) & 31U;
    if ((simd_three_same_vector || simd_three_same_scalar) &&
        simd_three_same_opcode < 24U) {
      if (!valid_simd_three_same_integer(instruction))
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_three_same_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9ff8'0400U) == 0x0f00'0400U) {
      const auto decoded =
          arm64::detail::decode_simd_modified_immediate(instruction);
      if (!decoded)
        return reject();
      if (!host_capabilities().advanced_simd)
        return Action::unsupported;
      emit_simd_modified_immediate(instruction, decoded->action != 0U);
      return Action::keep_going;
    }
    if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane || (!q && lane->element_shift == 3U))
        return reject();
      if (!host_capabilities().advanced_simd)
        return Action::unsupported;
      emit_simd_duplicate_general(instruction);
      return Action::keep_going;
    }
    const bool simd_insert_general =
        (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool simd_insert_element =
        (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (simd_insert_general || simd_insert_element) {
      if (!arm64::detail::decode_simd_lane_index(
              (instruction >> 16) & 31U))
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_insert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_duplicate_element_vector =
        (instruction & 0xbfe0'fc00U) == 0x0e00'0400U;
    const bool simd_duplicate_element_scalar =
        (instruction & 0xffe0'fc00U) == 0x5e00'0400U;
    if (simd_duplicate_element_vector || simd_duplicate_element_scalar) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane ||
          (simd_duplicate_element_vector && !q &&
           lane->element_shift == 3U))
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_duplicate_element(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto simd_bit_unary_operation = instruction & 0x9f3f'fc00U;
    const bool simd_bit_unary =
        simd_bit_unary_operation == 0x0e20'0800U ||
        simd_bit_unary_operation == 0x0e20'1800U ||
        simd_bit_unary_operation == 0x0e20'5800U;
    if (simd_bit_unary) {
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid =
          simd_bit_unary_operation == 0x0e20'0800U
              ? (alternate ? size < 2U : size < 3U)
          : simd_bit_unary_operation == 0x0e20'1800U
              ? !alternate && size == 0U
              : alternate ? size <= 1U : size == 0U;
      if (!valid)
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_bit_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_scalar_pairwise_add(instruction))
      return Action::keep_going;
    if ((instruction & 0x9f80'fc00U) == 0x0f00'a400U) {
      const unsigned encoded = (instruction >> 16) & 0x7fU;
      const unsigned source_bits = std::bit_floor(encoded);
      if (source_bits < 8U || source_bits > 32U ||
          encoded - source_bits >= source_bits)
        return reject();
      if (!host_capabilities().advanced_simd ||
          !emit_simd_shift_left_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_shift_immediate(instruction))
      return Action::keep_going;
    const bool vector_shift_immediate =
        (instruction & 0x9f00'0400U) == 0x0f00'0400U &&
        ((instruction >> 16) & 0x7fU) >= 8U;
    const bool scalar_shift_immediate =
        (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    const unsigned shift_opcode = (instruction >> 11) & 31U;
    const bool reserved_shift_opcode =
        shift_opcode < 20U ||
        (shift_opcode >= 21U && shift_opcode <= 27U) ||
        shift_opcode == 29U || shift_opcode == 30U ||
        (scalar_shift_immediate && shift_opcode == 20U);
    if ((vector_shift_immediate || scalar_shift_immediate) &&
        reserved_shift_opcode)
      return reject();
    const auto simd_compare_zero_class = instruction & 0x9f3f'fc00U;
    /* The scalar forms are op0 01x1; op0 00x1 is scalar floating point. */
    const auto scalar_compare_zero_class = instruction & 0xdf3f'fc00U;
    if (simd_compare_zero_class == 0x0e20'8800U ||
        simd_compare_zero_class == 0x0e20'9800U ||
        simd_compare_zero_class == 0x0e20'a800U ||
        scalar_compare_zero_class == 0x5e20'8800U ||
        scalar_compare_zero_class == 0x5e20'9800U ||
        scalar_compare_zero_class == 0x5e20'a800U) {
      const unsigned opcode = (instruction >> 12) & 31U;
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const bool scalar = (instruction & 0x1000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid_opcode =
          opcode == 8U || opcode == 9U || (opcode == 10U && !alternate);
      const bool valid_shape =
          (!scalar || size == 3U) && (scalar || q || size != 3U);
      if (!valid_opcode || !valid_shape || !host_capabilities().advanced_simd)
        return reject();
      emit_native_vector_unary(instruction, false);
      return Action::keep_going;
    }
    const auto simd_general_opcode = instruction & 0xffff'fc00U;
    const bool simd_general_class =
        (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
        (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U ||
        simd_general_opcode == 0x1ee6'0000U ||
        simd_general_opcode == 0x9ee6'0000U ||
        simd_general_opcode == 0x1ee7'0000U ||
        simd_general_opcode == 0x9ee7'0000U ||
        simd_general_opcode == 0x9e66'0000U ||
        simd_general_opcode == 0x9e67'0000U ||
        simd_general_opcode == 0x1e26'0000U ||
        simd_general_opcode == 0x1e27'0000U ||
        simd_general_opcode == 0x9eae'0000U ||
        simd_general_opcode == 0x9eaf'0000U;
    if (simd_general_class) {
      if (!host_capabilities().advanced_simd ||
          !emit_simd_general_move(instruction))
        return reject();
      return Action::keep_going;
    }
    /* STGP occupies the GPR opc=01 store allocation and needs FEAT_MTE. */
    if ((instruction & 0xffc0'0000U) == 0x6880'0000U ||
        (instruction & 0xffc0'0000U) == 0x6900'0000U ||
        (instruction & 0xffc0'0000U) == 0x6980'0000U)
      return reject();
    /* LDRAA and LDRAB need FEAT_PAuth, which the virtual CPU does not
     * implement. */
    if ((instruction & 0xff20'0400U) == 0xf820'0400U)
      return reject();
    /*
     * The virtual CPU implements none of FEAT_LS64, FEAT_LSFE, FEAT_LSE128,
     * FEAT_THE, FEAT_D128, and FEAT_LSUI. LD64B, ST64B, ST64BV, ST64BV0, the
     * floating-point atomics, LDCLRP, LDSETP, SWPP, RCWCAS, RCWCASP, the
     * read-check-write pair modifies, and the unprivileged atomics,
     * compare-and-swaps, and exclusives are undefined. The scalar
     * read-check-write modifies fall in cells the atomic decoder rejects.
     */
    if ((instruction & 0xffff'fc00U) == 0xf83f'd000U ||
        (instruction & 0xffff'fc00U) == 0xf83f'9000U ||
        (instruction & 0xffe0'fc00U) == 0xf820'b000U ||
        (instruction & 0xffe0'fc00U) == 0xf820'a000U ||
        (instruction & 0x3f20'0c00U) == 0x3c20'0000U ||
        (instruction & 0xbf20'0c00U) == 0x1920'0000U ||
        (instruction & 0xbf20'0800U) == 0x1920'0800U ||
        (instruction & 0x3f20'0c00U) == 0x1920'0400U ||
        (instruction & 0x3f00'0000U) == 0x0900'0000U)
      return reject();
    /*
     * MOPS shares major encodings with atomic memory instructions. Decode its
     * complete allocation first. Translation resolves every legality choice,
     * and every reserved encoding terminates here. The main stage transfers
     * nothing (arm64::detail::mops_prologue); a prologue or epilogue that the
     * compile loop could not fuse with its neighbours takes the helper alone.
     */
    if (const auto mops = arm64::detail::decode_mops(instruction);
        mops.recognized) {
      if (!mops.valid)
        return reject();
      if (mops.stage == 1U)
        return Action::keep_going;
      if (mops.stage == 2U)
        mark_memory_write();
      emit_memory_copy_set(instruction, pc, resulting_count, 1U);
      return Action::terminate;
    }
    if ((instruction & 0x3f20'0c00U) == 0x3820'0000U) {
      const auto action = emit_atomic_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      return reject();
    }
    if ((instruction & 0x3f00'0000U) == 0x0800'0000U) {
      const auto action = emit_exclusive_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      return reject();
    }
    if ((instruction & 0x3a00'0000U) == 0x2800'0000U) {
      const bool vector = (instruction & 0x0400'0000U) != 0;
      const bool load = (instruction & 0x0040'0000U) != 0;
      const unsigned operation = instruction >> 30;
      const unsigned mode = (instruction >> 23) & 3U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 10) & 31U;
      /* opc 11 holds the FEAT_LSUI LDTP, STTP, LDTNP, and STTNP, which the
       * virtual CPU does not implement. */
      const bool valid =
          operation != 3U &&
          (vector || (operation != 1U || (load && mode != 0U))) &&
          (!load || first != second);
      if (!valid)
        return reject();
      if (emit_pair_memory(instruction, pc))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
      if (emit_simd_structure_memory(instruction, pc, false))
        return Action::keep_going;
      return reject();
    }
    if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
        (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
      if (emit_simd_structure_memory(instruction, pc, true))
        return Action::keep_going;
      return reject();
    }
    if (emit_literal_memory(instruction, pc))
      return Action::keep_going;
    if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
      /*
       * The only allocation rejected by emit_literal_memory after the
       * host-feature gate is SIMD&FP opc=11. Resolve that reserved quadrant
       * while translating so it cannot become an unsupported runtime event.
       */
      return reject();
    }
    if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
      if (emit_other_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      const bool register_offset = (instruction & 0x0020'0000U) != 0U;
      const unsigned mode = (instruction >> 10) & 3U;
      const unsigned option = (instruction >> 13) & 7U;
      const bool invalid_vector =
          vector &&
          ((opc >= 2U && size_shift != 0U) ||
           (register_offset && (mode != 2U || (option & 2U) == 0U)) ||
           (!register_offset && mode == 2U));
      if (!vector || invalid_vector)
        return reject();
      return Action::unsupported;
    }
    if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
      if (emit_unsigned_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      if (!vector || (opc >= 2U && size_shift != 0U))
        return reject();
      return Action::unsupported;
    }
    return Action::unsupported;
  }

} // namespace uml::cpu::arm64
