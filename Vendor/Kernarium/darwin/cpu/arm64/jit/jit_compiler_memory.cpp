// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/detail/sm0_vector_length.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace uml::cpu::arm64 {

void JitCompiler::emit_translate(unsigned address, Access access,
                      Arm64Emitter::Label fallback, Arm64Emitter::Label fault,
                      std::size_t bytes, bool bridge) {
    if (bytes > 1) {
      emitter_.ubfm(0, address, 0, page_shift - 1);
      emitter_.add_immediate(0, 0, static_cast<unsigned>(bytes - 1));
      /*
       * bytes is translation-time bounded below one page, so the sum is less
       * than two pages. Its page bit is therefore the complete crossing test.
       */
      emitter_.test_zero(fallback, 0, page_shift, true);
    }

    emit_tlb_set(address);
    const unsigned page_offset = access == Access::write ? 16U : 0U;
    const auto slow = emitter_.make_label();
    const auto complete = emitter_.make_label();
    /*
     * The access-specific tag and backing delta are adjacent immutable fields.
     * Load both before matching the tag. The overwhelmingly common hit falls
     * through one normally-not-taken CBNZ and directly applies the
     * host-owned delta. The preserving miss bridge is emitted after the
     * complete hot block and branches back to the same continuation.
     */
    emitter_.load_pair_x(2, 9, 0, page_offset);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(slow, 2, true, true);
    emitter_.add_shifted(address, address, 9, 0);
    emitter_.bind(complete);
    translation_cold_.push_back({
        .label = slow,
        .continuation = complete,
        .fault = fault,
        .address = address,
        .access = access,
        .bridge = bridge,
    });
  }

void JitCompiler::emit_tlb_set(unsigned address) {
    /*
     * The TLB tag is the guest page number, as the shared
     * `uml::memory::TlbEntry` contract and the gadget engine both store it.
     * One shift produces it, and the backing delta the hit path applies is
     * still relative to the page-aligned address.
     */
    emitter_.ubfm(1, address, page_shift, 63U); // lsr x1, Xn, #page_shift
    emitter_.ubfm(0, address, page_shift,
                  page_shift + ::uml::memory::tlb_index_bits - 1U);
    emitter_.add_shifted(0, 14, 0, 5);
  }

void JitCompiler::emit_untag_data_address(unsigned address) {
    if (!top_byte_ignore())
      return;
    emitter_.ubfm(address, address, 0, 55);
  }

void JitCompiler::emit_translation_cold(const TranslationColdPath &path) {
    emitter_.bind(path.label);
    if (!path.bridge) {
      emitter_.branch(path.fault);
      return;
    }
    emitter_.move_x(0, 14);
    emitter_.move_x(1, path.address);
    emitter_.load_constant(
        2, path.access == Access::write ? 1U : 0U);
    /* The preserving bridge saves 128-bit vector halves, which would lose
     * a streaming translation's Z registers; its counterpart leaves and
     * re-enters streaming mode around the walker instead. */
    emitter_.call_preserving_flags(
        streaming_translation()
            ? reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_streaming)
            : reinterpret_cast<const void *>(
                  &uml_arm64_jit_tlb_translate_preserving));
    emitter_.move_x(path.address, 0);
    emitter_.compare_zero(path.fault, path.address, true, false);
    emitter_.branch(path.continuation);
  }

void JitCompiler::emit_misalignment_check(unsigned address, std::size_t alignment,
                               Arm64Emitter::Label misaligned) {
    if (alignment <= 1U)
      return;
    if (!std::has_single_bit(alignment) || alignment > 64U) {
      /* A malformed translation request must take the existing cold semantic
       * path for every execution; do not manufacture an invalid mask or abort
       * the emulator. All production callers use constants in [2, 64]. */
      emitter_.branch(misaligned);
      return;
    }
    emitter_.ubfm(
        0, address, 0,
        static_cast<unsigned>(std::countr_zero(alignment) - 1U));
    emitter_.compare_zero(misaligned, 0, true, true);
  }

/*
 * Ends the software monitor where the guest's pair ends: after a fused
 * store-exclusive and on a fused run's mismatch exit. An ordinary store of the
 * guest's own leaves it standing (see the monitor contract in
 * docs/architecture.md); reservation versions fail a store-exclusive that a
 * store to the reserved granule has beaten.
 */
void JitCompiler::clear_local_exclusive_monitor() {
    emitter_.store_b(31, 15, offsetof(CpuState, exclusive_size));
  }

[[nodiscard]] std::size_t
  JitCompiler::try_emit_native_exclusive_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    if (count_ != 0U || !gpr_slots_unbound() || instructions.size() < 2U)
      return 0;

    const auto load_instruction = instructions[0];
    const auto scalar_exclusive = [](std::uint32_t instruction) {
      return (instruction & 0x3fa0'0000U) == 0x0800'0000U;
    };
    if (!scalar_exclusive(load_instruction) ||
        (load_instruction & 0x0040'0000U) == 0U)
      return 0;

    std::size_t store_index = 1U;
    bool compare_retry = false;
    if (!scalar_exclusive(instructions[store_index]) ||
        (instructions[store_index] & 0x0040'0000U) != 0U) {
      if (instructions.size() < 3U)
        return 0;
      const auto middle = instructions[1];
      const bool add_sub_immediate =
          (middle & 0x1f80'0000U) == 0x1100'0000U;
      const bool add_sub_shifted =
          (middle & 0x1f20'0000U) == 0x0b00'0000U &&
          valid_shifted_register(middle, true);
      const bool logical_shifted =
          (middle & 0x1f00'0000U) == 0x0a00'0000U &&
          valid_shifted_register(middle, false);
      if (!add_sub_immediate && !add_sub_shifted && !logical_shifted)
        return 0;
      store_index = 2U;
      if (instructions.size() >= 5U &&
          (middle & 0x7fe0'fc1fU) == 0x6b00'001fU &&
          (instructions[2] & 0xff00'001fU) == 0x5400'0001U) {
        compare_retry = true;
        store_index = 3U;
      }
    }

    const auto store_instruction = instructions[store_index];
    if (!scalar_exclusive(store_instruction) ||
        (store_instruction & 0x0040'0000U) != 0U)
      return 0;

    const unsigned size = load_instruction >> 30;
    const unsigned base = (load_instruction >> 5) & 31U;
    const unsigned load_destination = load_instruction & 31U;
    const unsigned store_source = store_instruction & 31U;
    const unsigned status = (store_instruction >> 16) & 31U;
    if ((store_instruction >> 30) != size ||
        ((store_instruction >> 5) & 31U) != base ||
        ((load_instruction >> 10) & 31U) != 31U ||
        ((load_instruction >> 16) & 31U) != 31U ||
        ((store_instruction >> 10) & 31U) != 31U ||
        load_destination == base)
      return 0;

    if (compare_retry) {
      const auto compare = instructions[1];
      const auto mismatch_offset =
          sign_extend((instructions[2] >> 5) & 0x7ffffU, 19) * 4;
      const auto retry = instructions[4];
      const auto retry_offset = sign_extend((retry >> 5) & 0x7ffffU, 19) * 4;
      if (load_destination == 31U || status == load_destination ||
          ((compare >> 5) & 31U) != load_destination ||
          (compare >> 31) != (size == 3U ? 1U : 0U) ||
          mismatch_offset < 12 ||
          (retry & 0xff00'001fU) != (0x3500'0000U | status) ||
          retry_offset != -16)
        return 0;
    }

    const bool has_middle = store_index == 2U || compare_retry;
    const auto middle_instruction =
        has_middle ? instructions[1] : std::uint32_t{};
    if (has_middle && (middle_instruction & 31U) == base)
      return 0;

    /* This fusion needs at most six distinct resident architectural values.
     * Prove capacity before emitting any code so guest-selected register
     * aliases can only decline the optimization, never exhaust the allocator
     * after the native exclusive interval has been constructed. */
    std::array<bool, 32> required_gprs{};
    std::size_t required_gpr_count = 0;
    const auto require_gpr = [&](unsigned guest, bool stack_pointer) {
      if (guest == 31U && !stack_pointer)
        return;
      if (!required_gprs[guest]) {
        required_gprs[guest] = true;
        ++required_gpr_count;
      }
    };
    if (has_middle) {
      const auto left = (middle_instruction >> 5) & 31U;
      const auto right = (middle_instruction >> 16) & 31U;
      const bool immediate =
          (middle_instruction & 0x1f80'0000U) == 0x1100'0000U;
      const bool writes_flags =
          (middle_instruction & 0x2000'0000U) != 0U;
      if (left != load_destination)
        require_gpr(left, immediate);
      if (!immediate && right != load_destination)
        require_gpr(right, false);
      require_gpr(middle_instruction & 31U,
                  immediate && !writes_flags);
    }
    if (store_source != load_destination &&
        (!has_middle || store_source != (middle_instruction & 31U)))
      require_gpr(store_source, false);
    require_gpr(status, false);
    require_gpr(load_destination, false);
    if (required_gpr_count > gpr_slot_capacity())
      return 0;

    const std::size_t bytes = std::size_t{1} << size;
    const auto slow = cold_exit(load_instruction, pc,
                                ColdPath::Action::deferred_exclusive_slow);
    const auto fault =
        cold_exit(load_instruction, pc, ColdPath::Action::memory_fault);

    load_gpr(8, base, true);
    if (base == 31U)
      emit_misalignment_check(8, 16, slow);
    emit_untag_data_address(8);
    emit_misalignment_check(8, bytes, slow);

    /*
     * Resolve both permissions before establishing the host monitor. x21 is
     * host-owned chain state, so it can retain the translated write address
     * across the one register-only middle instruction. The selector is
     * restored after the store, before the terminating branch can use it.
     */
    emit_translate(8, Access::write, slow, slow, bytes);
    emitter_.move_x(21, 8);
    /*
     * A native conditional store still participates in the publication
     * protocol. Record its possible range before establishing the host
     * monitor; invalidation on a failed attempt is conservative and avoids
     * putting helper work or a status branch inside the native interval.
     */
    track_direct_store(21, bytes, count_ + store_index);
    load_gpr(8, base, true);
    emit_untag_data_address(8);
    emit_translate(8, Access::read, slow, fault, bytes);
    emitter_.compare_register(8, 21);
    emitter_.branch_condition(slow, 1); // NE

    /*
     * Bind every register needed between the host exclusive load/store before
     * establishing the monitor. The emitted interval then contains only the
     * native load, an optional translation-selected integer operation, and
     * the native store; no CpuState access or helper call can intervene.
     */
    const auto preload = [&](unsigned guest, bool stack_pointer) {
      (void)read_native_gpr(guest, stack_pointer, 10);
    };
    if (has_middle) {
      const auto left = (middle_instruction >> 5) & 31U;
      const auto right = (middle_instruction >> 16) & 31U;
      const bool immediate =
          (middle_instruction & 0x1f80'0000U) == 0x1100'0000U;
      if (left != load_destination)
        preload(left, immediate);
      if (!immediate && right != load_destination)
        preload(right, false);
      const bool writes_flags =
          (middle_instruction & 0x2000'0000U) != 0U;
      const auto destination =
          write_native_gpr(middle_instruction & 31U,
                           immediate && !writes_flags, false, 10);
      (void)destination;
    }
    if (store_source != load_destination &&
        (!has_middle || store_source != (middle_instruction & 31U)))
      preload(store_source, false);
    const auto preallocated_status =
        write_native_gpr(status, false, compare_retry, 9);

    const auto native_destination =
        write_native_gpr(load_destination, false, false, 9);
    auto native_load =
        load_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_load |= (31U << 16) | (31U << 10) | (21U << 5) |
                   native_destination.host;
    emitter_.emit(native_load);
    commit_native_gpr(native_destination, load_destination, false);

    ++count_;
    if (has_middle) {
      const auto middle_action =
          emit_instruction(middle_instruction, pc + sizeof(std::uint32_t));
      /* The predecoder above accepts only baseline add/sub or logical forms;
       * each is a nonterminal native lowering. */
      (void)middle_action;
      ++count_;
    }
    if (compare_retry) {
      const auto target = pc + 8U +
          sign_extend((instructions[2] >> 5) & 0x7ffffU, 19) * 4;
      const auto mismatch =
          cold_exit(instructions[2], target,
                    ColdPath::Action::exclusive_compare_mismatch);
      load_flags();
      emitter_.branch_condition(mismatch, 1); // NE
      ++count_;
    }
    mark_direct_memory_write();
    auto native_source = read_native_gpr(store_source, false, 10);
    const auto native_status = preallocated_status;
    if (status != 31U && status == store_source) {
      emitter_.move_x(10, native_source.host);
      native_source = {10U, false};
    }
    auto native_store =
        store_instruction &
        ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
    native_store |= (native_status.host << 16) | (31U << 10) |
                    (21U << 5) | native_source.host;
    emitter_.emit(native_store);
    commit_native_gpr(native_status, status, false);
    clear_local_exclusive_monitor();
    emitter_.load_b(21, 19, offsetof(JitChainState, link_selector));

    ++count_;
    return store_index + 1U;
  }

[[nodiscard]] JitCompiler::Action JitCompiler::emit_exclusive_memory(std::uint32_t instruction,
                                             std::uint64_t pc) {
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool pair = (instruction & 0x0020'0000U) != 0;
    const bool exclusive = (instruction & 0x0080'0000U) == 0;
    const unsigned status = (instruction >> 16) & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const unsigned size = instruction >> 30;
    const bool pair_compare_swap =
        (instruction & 0xbfa0'0000U) == 0x0820'0000U;
    const bool scalar_compare_swap =
        (instruction & 0x3fa0'0000U) == 0x08a0'0000U;
    const bool pair_exclusive =
        (instruction & 0xbfa0'0000U) == 0x8820'0000U;
    const bool scalar_exclusive =
        (instruction & 0x3fa0'0000U) == 0x0800'0000U;

    bool valid = false;
    if (pair_compare_swap) {
      const unsigned replacement = instruction & 31U;
      valid = second == 31U && (status & 1U) == 0U &&
              (replacement & 1U) == 0U && size <= 1U;
    } else if (scalar_compare_swap) {
      valid = second == 31U;
    } else if (pair_exclusive) {
      const unsigned first = instruction & 31U;
      valid = size >= 2U &&
              (!load || (status == 31U && first != second));
    } else if (scalar_exclusive) {
      valid = second == 31U && (!load || status == 31U);
    } else {
      /* The remaining allocation is the non-exclusive ordered scalar
       * load/store family. Preserve its dedicated helper. o0 clear selects
       * LDLAR and STLLR, which need FEAT_LOR; the virtual CPU does not
       * implement it. */
      valid = !pair && second == 31U && status == 31U && !exclusive &&
              (instruction & 0x0000'8000U) != 0U;
      if (!valid)
        return Action::unsupported;
      if (load) {
        emit_native_ordered_load(instruction, pc,
                                 ColdPath::Action::ordered_slow);
        return Action::keep_going;
      }
      emitter_.branch(
          cold_exit(instruction, pc, ColdPath::Action::ordered_slow));
      return Action::terminate;
    }
    if (!valid)
      return Action::unsupported;
    if (scalar_compare_swap && host_capabilities().has(ArmFeature::lse)) {
      emit_native_scalar_compare_swap(instruction, pc);
      return Action::keep_going;
    }
    if (pair_compare_swap || scalar_compare_swap || !load)
      mark_memory_write();
    emitter_.branch(
        cold_exit(instruction, pc, ColdPath::Action::exclusive_slow));
    return Action::terminate;
  }

[[nodiscard]] JitCompiler::Action JitCompiler::emit_atomic_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const unsigned o3 = (instruction >> 15) & 1U;
    const unsigned opcode = (instruction >> 12) & 7U;
    if (o3 && opcode != 0U)
      return Action::unsupported;
    if (host_capabilities().has(ArmFeature::lse)) {
      emit_native_scalar_atomic(instruction, pc);
      return Action::keep_going;
    }
    mark_memory_write();
    emitter_.branch(
        cold_exit(instruction, pc, ColdPath::Action::exclusive_slow));
    return Action::terminate;
  }

void JitCompiler::emit_native_atomic_address(std::uint32_t instruction,
                                              std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const std::size_t bytes = std::size_t{1} << (instruction >> 30);
    load_gpr(8, base, true);

    const auto slow =
        cold_exit(instruction, pc, ColdPath::Action::exclusive_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    emit_untag_data_address(8);
    emit_misalignment_check(8, base == 31U ? 16 : bytes, slow);
    emit_translate(8, Access::write, slow, fault, 1);
    track_direct_store(8, bytes);
  }

void JitCompiler::emit_native_ordered_load(std::uint32_t instruction,
                                           std::uint64_t pc,
                                           ColdPath::Action slow_action) {
    /*
     * A naturally aligned access lies in one page, and the host's LDAR or
     * LDAPR on its backing is single-copy atomic with the same ordering.
     * A misaligned one keeps the semantic helper, which raises the
     * alignment fault or, with FEAT_LSE2, reads it atomically.
     */
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const std::size_t bytes = std::size_t{1} << (instruction >> 30);
    load_gpr(8, base, true);

    const auto slow = cold_exit(instruction, pc, slow_action);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    emit_untag_data_address(8);
    emit_misalignment_check(8, base == 31U ? 16 : bytes, slow);
    emit_translate(8, Access::read, slow, fault, 1);
    const auto native_destination =
        write_native_gpr(destination, false, false, 9);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) | (8U << 5) |
                  native_destination.host);
    commit_native_gpr(native_destination, destination, false);
  }

void JitCompiler::emit_native_scalar_atomic(std::uint32_t instruction,
                                 std::uint64_t pc) {
    emit_native_atomic_address(instruction, pc);
    const unsigned source = (instruction >> 16) & 31U;
    const unsigned destination = instruction & 31U;

    /*
     * Rs is consumed before Rt is defined, so a guest register may legally
     * name both operands. Translation also keeps the guest base value away
     * from the host pointer in x8.
     */
    const auto native_source = read_native_gpr(source, false, 9);
    const auto native_destination =
        write_native_gpr(destination, false, false, 10);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_source.host << 16) | (8U << 5) |
              native_destination.host;
    emitter_.emit(native);
    commit_native_gpr(native_destination, destination, false);
  }

void JitCompiler::emit_native_scalar_compare_swap(std::uint32_t instruction,
                                       std::uint64_t pc) {
    emit_native_atomic_address(instruction, pc);
    const unsigned compare = (instruction >> 16) & 31U;
    const unsigned replacement = instruction & 31U;

    /*
     * CAS reads both guest operands before publishing the old memory value
     * through the compare register. Keep distinct nonresident scratch
     * registers, while preserving the valid compare==replacement case.
     */
    const auto native_compare =
        read_native_gpr(compare, false, 9);
    const auto native_replacement =
        replacement == compare
            ? native_compare
            : read_native_gpr(replacement, false, 10);
    const auto native_result =
        write_native_gpr(compare, false, false,
                         native_compare.host);
    auto native =
        instruction & ~((31U << 16) | (31U << 5) | 31U);
    native |= (native_result.host << 16) | (8U << 5) |
              native_replacement.host;
    emitter_.emit(native);
    commit_native_gpr(native_result, compare, false);
  }

[[nodiscard]] bool JitCompiler::emit_pair_memory(std::uint32_t instruction,
                                      std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if (vector && !host_capabilities().floating_point)
      return false;
    if (!load)
      mark_direct_memory_write();

    const unsigned mode = (instruction >> 23) & 3U;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const unsigned second = (instruction >> 10) & 31U;
    const std::size_t bytes =
        vector            ? (operation >= 2U ? 16U
                                             : std::size_t{4} << operation)
        : operation >= 2U ? 8U
                          : 4U;
    const auto displacement =
        sign_extend((instruction >> 15) & 0x7fU, 7) *
        static_cast<std::int64_t>(bytes);
    const auto emit_displacement = [&](unsigned destination,
                                       unsigned source) {
      if (displacement == 0)
        return;
      const auto magnitude = static_cast<unsigned>(
          displacement < 0 ? -displacement : displacement);
      emitter_.emit((displacement < 0 ? 0xd100'0000U : 0x9100'0000U) |
                    (magnitude << 10) | (source << 5) | destination);
    };

    const auto native_base = read_native_gpr(base, true, 8);
    if (native_base.host != 8U)
      emitter_.move_x(8, native_base.host);
    const auto slow = cold_exit(instruction, pc, ColdPath::Action::pair_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    if (base == 31U) {
      /* Pair accesses through SP require the architectural stack alignment
       * check before either memory or writeback can change. */
      emit_misalignment_check(8, 16, slow);
    }
    if (mode != 1U)
      emit_displacement(8, 8);
    emit_untag_data_address(8);

    /* The helper remains responsible for cross-page atomicity. The common
     * same-page case needs one TLB lookup and one native pair instruction. */
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   bytes * 2U);
    if (!load)
      track_direct_store(8, bytes * 2U);
    NativeGpr native_first_gpr{};
    NativeGpr native_second_gpr{};
    NativeVector native_first_vector{};
    NativeVector native_second_vector{};
    if (vector) {
      if (load) {
        native_first_vector = write_native_vector(first, false, 0);
        native_second_vector =
            second == first
                ? native_first_vector
                : write_native_vector(second, false, 1);
      } else {
        native_first_vector = read_native_vector(first, 0);
        native_second_vector =
            second == first ? native_first_vector
                            : read_native_vector(second, 1);
      }
    } else {
      if (load) {
        native_first_gpr = write_native_gpr(first, false, false, 9);
        native_second_gpr =
            second == first
                ? native_first_gpr
                : write_native_gpr(second, false, false, 10);
      } else {
        native_first_gpr = read_native_gpr(first, false, 9);
        native_second_gpr =
            second == first ? native_first_gpr
                            : read_native_gpr(second, false, 10);
      }
    }

    /* Address generation and writeback were resolved while translating.
     * Preserve the pair operation/mode, replace its immediate with zero, and
     * name the operands' resident host bindings directly. A full cache falls
     * back to the scratch registers x9 and x10, or v0 and v1. */
    auto native =
        instruction & ~((0x7fU << 15) | (31U << 10) | (31U << 5) | 31U);
    native |=
        (8U << 5) |
        ((vector ? native_second_vector.host : native_second_gpr.host) << 10) |
        (vector ? native_first_vector.host : native_first_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector) {
        commit_native_vector(native_first_vector, first);
        if (second != first)
          commit_native_vector(native_second_vector, second);
      } else {
        commit_native_gpr(native_first_gpr, first, false);
        if (second != first)
          commit_native_gpr(native_second_gpr, second, false);
      }
    }

    const bool writeback =
        (mode == 1U || mode == 3U) &&
        !(load && !vector && base != 31U &&
          (base == first || base == second));
    if (writeback && displacement != 0) {
      const auto native_writeback =
          write_native_gpr(base, true, true, 9);
      emit_displacement(native_writeback.host, native_writeback.host);
      commit_native_gpr(native_writeback, base, true);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_structure_memory(std::uint32_t instruction,
                                                std::uint64_t pc, bool single) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool load = (instruction & 0x0040'0000U) != 0;
    const bool post_index = (instruction & 0x0080'0000U) != 0;
    const unsigned offset_register = (instruction >> 16) & 31U;
    if (!post_index && offset_register != 0U)
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned first = instruction & 31U;
    const std::size_t vector_size =
        (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
    unsigned register_count = 0;
    std::size_t element_size = 0;
    bool load_preserves_destination = false;
    if (!single) {
      const unsigned opcode = (instruction >> 12) & 15U;
      const bool interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
      register_count = opcode == 7U    ? 1U
                       : opcode == 10U ? 2U
                       : opcode == 6U  ? 3U
                       : opcode == 2U  ? 4U
                       : opcode == 8U  ? 2U
                       : opcode == 4U  ? 3U
                       : opcode == 0U  ? 4U
                                       : 0U;
      const unsigned size = (instruction >> 10) & 3U;
      if (register_count == 0U ||
          (interleaved && vector_size == 8U && size == 3U))
        return false;
      element_size = std::size_t{1} << size;
    } else {
      const unsigned extra_register = (instruction >> 21) & 1U;
      const unsigned opcode = (instruction >> 13) & 7U;
      const unsigned scale_bit = (instruction >> 12) & 1U;
      const unsigned size = (instruction >> 10) & 3U;
      const unsigned scale = opcode >> 1;
      if (scale == 3U) {
        if (!load || scale_bit != 0U)
          return false;
        register_count =
            opcode == 6U ? 1U + extra_register : 3U + extra_register;
        element_size = std::size_t{1} << size;
      } else {
        load_preserves_destination = load;
        register_count =
            (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
        if (scale == 0U) {
          element_size = 1U;
        } else if (scale == 1U) {
          if ((size & 1U) != 0U)
            return false;
          element_size = 2U;
        } else {
          if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
            return false;
          element_size = (size & 1U) != 0U ? 8U : 4U;
        }
      }
    }
    const auto transfer_size =
        single ? element_size * register_count : vector_size * register_count;
    if (!load)
      mark_direct_memory_write();

    load_gpr(8, base, true);
    const auto slow =
        cold_exit(instruction, pc, ColdPath::Action::simd_structure_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    if (base == 31U) {
      emit_misalignment_check(8, 16, slow);
    }
    emit_untag_data_address(8);
    emit_translate(8, load ? Access::read : Access::write, slow, fault,
                   transfer_size);
    if (!load)
      track_direct_store(8, transfer_size);
    NativeVector native_vector{};
    if (register_count == 1U) {
      /*
       * A one-register structure operation has an unrestricted Rt field.
       * Keep the guest vector in its resident v16-v31 binding and name that
       * binding in the original instruction.  Selection and validity were
       * resolved above while translating; the generated memory hot path
       * therefore gains no opcode checks or dispatch branches.
       *
       * Lane loads read the untouched lanes, while full and replicate loads
       * overwrite the architectural destination.  A full vector cache uses
       * v0 and the existing commit path exactly as the staged lowering did.
       */
      native_vector =
          load ? write_native_vector(first, load_preserves_destination, 0)
               : read_native_vector(first, 0);
    } else if (!load || load_preserves_destination) {
      for (unsigned index = 0; index < register_count; ++index)
        load_q(index, 15,
               offsetof(CpuState, vectors) +
                   ((first + index) & 31U) *
                       sizeof(arm64::VectorRegister));
    }
    const auto native =
        (instruction & ~(0x0080'0000U | (31U << 16) | (31U << 5) | 31U)) |
        (8U << 5) |
        (register_count == 1U ? native_vector.host : 0U);
    emitter_.emit(native);
    if (load) {
      if (register_count == 1U) {
        commit_native_vector(native_vector, first);
      } else {
        for (unsigned index = 0; index < register_count; ++index)
          store_q(index, 15,
                  offsetof(CpuState, vectors) +
                      ((first + index) & 31U) *
                          sizeof(arm64::VectorRegister));
      }
    }
    if (post_index) {
      load_gpr(8, base, true);
      if (offset_register == 31U)
        emitter_.load_constant(9, transfer_size);
      else
        load_gpr(9, offset_register, false);
      emitter_.add_shifted(8, 8, 9, 0);
      store_gpr(8, base, true);
    }
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_other_memory(std::uint32_t instruction,
                                       std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_capabilities().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    const bool register_offset = (instruction & 0x0020'0000U) != 0;
    const unsigned mode = (instruction >> 10) & 3U;
    const unsigned option = (instruction >> 13) & 7U;
    if (!vector && size_shift == 3U && opc == 2U)
      /* PRFUM/register-offset PRFM are architecturally
       * non-faulting hints. All address work disappears at
       * translation time after validating their encoding. */
      return register_offset ? mode == 2U && (option & 2U) != 0U : mode == 0U;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    if ((register_offset && (mode != 2U || (option & 2U) == 0U)) ||
        (!register_offset && vector && mode == 2U))
      return false;
    const bool load = vector ? (opc & 1U) != 0U : opc != 0U;
    if (!load)
      mark_direct_memory_write();
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto displacement = sign_extend((instruction >> 12) & 0x1ffU, 9);

    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment =
          cold_exit(instruction, pc, ColdPath::Action::alignment_fault);
      /* Only the translation-selected SP path pays this architectural
       * alignment branch. */
      emit_misalignment_check(8, 16, alignment);
    }
    if (register_offset) {
      const unsigned offset_register = (instruction >> 16) & 31U;
      const unsigned shift =
          (instruction & 0x1000U) != 0U
              ? static_cast<unsigned>(vector && opc >= 2U ? 4U : size_shift)
              : 0U;
      load_gpr(9, offset_register, false);
      emitter_.emit(0x8b20'0000U | (option << 13) | (shift << 10) | (9U << 16) |
                    (8U << 5) | 8U);
    } else if (mode != 1U && displacement != 0) {
      emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
      emitter_.add_shifted(8, 8, 9, 0);
    }

    const auto fallback =
        cold_exit(instruction, pc, ColdPath::Action::memory_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    emit_untag_data_address(8);
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native =
        0x3900'0000U |
        (instruction & (0xc000'0000U | 0x0400'0000U | 0x00c0'0000U)) |
        (8U << 5) | (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    }

    const bool writeback =
        !register_offset && (mode == 1U || mode == 3U) &&
        !(load && !vector && base == transfer && base != 31U);
    if (writeback) {
      load_gpr(8, base, true);
      if (displacement != 0) {
        emitter_.load_constant(9, static_cast<std::uint64_t>(displacement));
        emitter_.add_shifted(8, 8, 9, 0);
      }
      store_gpr(8, base, true);
    }
    return true;
  }

/*
 * A spill word of the SM = 0 vector-length family
 * (detail/sm0_vector_length.hpp): the 16-byte SIMD&FP access of the V
 * register that is Z, or the halfword access of the zero register that is a
 * predicate register's, with the signed byte offset added to the base. It
 * is emit_other_memory's unscaled access without the encoding's 9-bit limit
 * on the offset, so its slow paths (a misaligned SP, a page crossing) run
 * the word through the semantic library, which has no such limit either.
 */
void JitCompiler::emit_sm0_spill(const detail::Sm0Spill &spill,
                                 std::uint32_t instruction, std::uint64_t pc) {
  const bool predicate = spill.predicate;
  const bool load = !spill.store;
  const std::size_t bytes = spill.bytes();
  if (!load)
    mark_direct_memory_write();
  if (!predicate)
    accesses_fp_state_ = true;

  const unsigned base = spill.base;
  const auto slow = cold_exit(instruction, pc, ColdPath::Action::sm0_spill_slow);
  load_gpr(8, base, true);
  if (base == 31U)
    emit_misalignment_check(8, 16, slow);
  if (spill.offset != 0) {
    emitter_.load_constant(
        9, static_cast<std::uint64_t>(static_cast<std::int64_t>(spill.offset)));
    emitter_.add_shifted(8, 8, 9, 0);
  }
  const auto fault = cold_exit(instruction, pc, ColdPath::Action::memory_fault);
  emit_untag_data_address(8);
  emit_translate(8, load ? Access::read : Access::write, slow, fault, bytes);
  if (!load)
    track_direct_store(8, bytes);
  const auto native_vector =
      !predicate ? (load ? write_native_vector(spill.transfer, false, 0)
                         : read_native_vector(spill.transfer, 0))
                 : NativeVector{};
  const auto native_gpr =
      predicate ? (load ? write_native_gpr(31U, false, false, 9)
                        : read_native_gpr(31U, false, 9))
                : NativeGpr{};
  /* LDR or STR (immediate, unsigned offset) Qt or Wt, [x8]: the halfword
   * access is the size-1 form with V clear. */
  const std::uint32_t access =
      predicate ? 0x7900'0000U | (load ? 0x0040'0000U : 0U)
                : 0x3d00'0000U | (load ? 0x00c0'0000U : 0x0080'0000U);
  emitter_.emit(access | (8U << 5) |
                (predicate ? native_gpr.host : native_vector.host));
  if (load) {
    if (!predicate)
      commit_native_vector(native_vector, spill.transfer);
    else
      commit_native_gpr(native_gpr, 31U, false);
  }
}

[[nodiscard]] bool JitCompiler::emit_literal_memory(std::uint32_t instruction,
                                         std::uint64_t pc) {
    if ((instruction & 0xff00'0000U) == 0xd800'0000U)
      /* Literal PRFM is an architecturally non-faulting hint. */
      return true;
    if ((instruction & 0x3b00'0000U) != 0x1800'0000U)
      return false;
    const bool vector = (instruction & 0x0400'0000U) != 0;
    const unsigned operation = instruction >> 30;
    if ((vector && !host_capabilities().floating_point) ||
        (vector && operation == 3U))
      return false;
    const std::size_t bytes = vector            ? std::size_t{4} << operation
                              : operation == 1U ? 8U
                                                : 4U;
    const auto displacement =
        sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
    const auto address = pc + static_cast<std::uint64_t>(displacement);
    const unsigned destination = instruction & 31U;
    emitter_.load_constant(8, address);
    const auto fallback =
        cold_exit(instruction, pc, ColdPath::Action::memory_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    emit_translate(8, Access::read, fallback, fault, bytes);
    if (vector) {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xbd40'0100U, // ldr s0, [x8]
          0xfd40'0100U, // ldr d0, [x8]
          0x3dc0'0100U, // ldr q0, [x8]
      };
      const auto native_destination =
          write_native_vector(destination, false, 0);
      emitter_.emit(loads[operation] | native_destination.host);
      commit_native_vector(native_destination, destination);
    } else {
      static constexpr std::array<std::uint32_t, 3> loads{
          0xb940'0109U, // ldr w9, [x8]
          0xf940'0109U, // ldr x9, [x8]
          0xb980'0109U, // ldrsw x9, [x8]
      };
      const auto native_destination =
          write_native_gpr(destination, false, false, 9);
      emitter_.emit((loads[operation] & ~31U) | native_destination.host);
      commit_native_gpr(native_destination, destination, false);
    }
    return true;
  }

namespace {

/* The run collect_unsigned_run() found: up to maximum_run operations on one
 * base and in one direction, and the span of their offsets. */
template <std::size_t maximum_run, typename Operation>
struct UnsignedRun final {
  std::array<Operation, maximum_run> operations{};
  std::size_t length{};
  unsigned base{};
  bool load{};
  std::uint64_t first_offset{};
  std::uint64_t maximum_end{};
};

/* The leading instructions that `decode` accepts as unsigned-offset
 * operations on the first one's base and in its direction, at most
 * maximum_run of them, cut from the end until their furthest bytes lie
 * within one page. A length under two is no run. */
template <std::size_t maximum_run, typename Operation, typename Decode>
[[nodiscard]] UnsignedRun<maximum_run, Operation>
collect_unsigned_run(std::span<const std::uint32_t> instructions,
                     Decode decode) {
  UnsignedRun<maximum_run, Operation> run;
  std::array<std::uint64_t, maximum_run> first_offsets{};
  std::array<std::uint64_t, maximum_run> maximum_ends{};
  std::size_t candidates = 0;
  std::uint64_t first_offset = 0;
  std::uint64_t maximum_end = 0;
  while (candidates < run.operations.size() &&
         candidates < instructions.size()) {
    Operation operation{};
    if (!decode(instructions[candidates], operation))
      break;
    if (candidates == 0) {
      run.base = operation.base;
      run.load = operation.load;
      first_offset = operation.offset;
    } else if (operation.base != run.base || operation.load != run.load) {
      break;
    }
    first_offset = std::min(first_offset, operation.offset);
    maximum_end = std::max(maximum_end, operation.offset + operation.bytes);
    run.operations[candidates] = operation;
    first_offsets[candidates] = first_offset;
    maximum_ends[candidates] = maximum_end;
    ++candidates;
  }

  std::size_t length = candidates;
  while (length >= 2 &&
         maximum_ends[length - 1] - first_offsets[length - 1] > page_size)
    --length;
  if (length >= 2) {
    run.length = length;
    run.first_offset = first_offsets[length - 1];
    run.maximum_end = maximum_ends[length - 1];
  }
  return run;
}

} // namespace

[[nodiscard]] bool JitCompiler::decode_simd_unsigned_memory(
      std::uint32_t instruction,
      UnsignedMemoryOperation &operation) noexcept {
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) == 0U)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if (opc >= 2U && size_shift != 0U)
      return false;

    const std::size_t bytes =
        opc >= 2U ? 16U : std::size_t{1} << size_shift;
    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU) *
                  bytes,
        .bytes = bytes,
        .base = (instruction >> 5) & 31U,
        .transfer = instruction & 31U,
        .load = (opc & 1U) != 0U,
    };
    return true;
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_simd_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 8;
    if (track_direct_stores_ || !host_capabilities().floating_point ||
        instructions.size() < 2)
      return 0;

    const auto [operations, length, base, load, first_offset, maximum_end] =
        collect_unsigned_run<maximum_run, UnsignedMemoryOperation>(
            instructions, decode_simd_unsigned_memory);
    if (length < 2)
      return 0;

    /*
     * Every guard and the permission lookup precedes the first architectural
     * memory operation.  A rejected proof therefore enters the ordinary
     * semantic path for operation zero with the complete dirty prefix still
     * visible, preserving its effective fault address and retirement count.
     */
    const auto slow = cold_exit(operations[0].instruction, pc,
                                ColdPath::Action::memory_slow);

    Arm64Emitter::Label alignment{};
    if (base == 31U)
      alignment = cold_exit(operations[0].instruction, pc,
                            ColdPath::Action::alignment_fault);

    load_gpr(8, base, true);
    if (base == 31U) {
      /* All retained operations name SP, so its architectural alignment is
       * checked once before any destination or memory becomes observable. */
      emit_misalignment_check(8, 16, alignment);
    }

    RunStraddle straddle{
        .label = emitter_.make_label(),
        .slow = slow,
        .access = load ? Access::read : Access::write,
        .first = first_offset,
    };
    bool proved = false;
    if (base == 31U && first_offset == 0U) {
      const auto last_unit =
          static_cast<unsigned>((maximum_end - 1U) >> 4U);
      if (last_unit != 0U) {
        /*
         * SP is 16-byte aligned.  Prove the furthest retained byte remains in
         * the page before translating SP itself once.
         */
        emitter_.ubfm(0, 8, 4, page_shift - 1);
        emitter_.add_immediate(0, 0, last_unit);
        emitter_.test_zero(straddle.label, 0, page_shift - 4U, true);
        proved = true;
      }
      emit_untag_data_address(8);
    } else {
      proved = emit_run_span_proof(first_offset, maximum_end, straddle.label);
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    move_run_base(first_offset, false);
    straddle.words_begin = emitter_.size();
    for (std::size_t index = 0; index < length; ++index) {
      const auto &operation = operations[index];
      const auto native_vector =
          load ? write_native_vector(operation.transfer, false, 0)
               : read_native_vector(operation.transfer, 0);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_vector.host;
      note_run_straddle_access(straddle, operation.offset, operation.bytes);
      emitter_.emit(native);
      if (load)
        commit_native_vector(native_vector, operation.transfer);
    }
    straddle.words_end = emitter_.size();
    if (proved) {
      straddle.join = emitter_.make_label();
      emitter_.bind(straddle.join);
      run_straddles_.push_back(straddle);
    }

    if (!load)
      mark_direct_memory_writes(length);
    return length;
  }

[[nodiscard]] bool JitCompiler::decode_scalar_unsigned_memory(
      std::uint32_t instruction, bool sp_base,
      UnsignedMemoryOperation &operation) noexcept {
    const unsigned base = (instruction >> 5) & 31U;
    if ((instruction & 0x3b00'0000U) != 0x3900'0000U ||
        (instruction & 0x0400'0000U) != 0U || (base == 31U) != sp_base)
      return false;

    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    if ((size_shift == 3U && opc >= 2U) ||
        (size_shift == 2U && opc == 3U))
      return false;

    operation = {
        .instruction = instruction,
        .offset = static_cast<std::uint64_t>(
                      (instruction >> 10) & 0xfffU)
                  << size_shift,
        .bytes = std::size_t{1} << size_shift,
        .base = base,
        .transfer = instruction & 31U,
        .load = opc != 0U,
    };
    return true;
  }

void JitCompiler::emit_native_gpr_run(
      std::span<const UnsignedMemoryOperation> operations, bool load,
      RunStraddle &straddle, bool proved) {
    straddle.words_begin = emitter_.size();
    for (const auto &operation : operations) {
      const auto native_gpr =
          load ? write_native_gpr(operation.transfer, false, false, 9)
               : read_native_gpr(operation.transfer, false, 9);
      const auto native =
          (operation.instruction & ~((31U << 5) | 31U)) | (8U << 5) |
          native_gpr.host;
      note_run_straddle_access(straddle, operation.offset, operation.bytes);
      emitter_.emit(native);
      if (load)
        commit_native_gpr(native_gpr, operation.transfer, false);
    }
    straddle.words_end = emitter_.size();
    if (proved) {
      straddle.join = emitter_.make_label();
      emitter_.bind(straddle.join);
      run_straddles_.push_back(straddle);
    }
  }

void JitCompiler::note_run_straddle_access(RunStraddle &straddle,
                                           std::uint64_t offset,
                                           std::size_t bytes) {
    if (straddle.count == straddle.accesses.size())
      return;
    const auto start = offset - straddle.first;
    straddle.accesses[straddle.count++] = {
        .word = emitter_.size(),
        .start = start,
        .end = start + bytes,
    };
  }

void JitCompiler::move_run_base(std::uint64_t offset, bool forward,
                                unsigned reg) {
    /* An unsigned offset is below 0x1000 units of at most 16 bytes. */
    const auto high = static_cast<unsigned>(offset >> 12U);
    const auto low = static_cast<unsigned>(offset & 0xfffU);
    if (high != 0U) {
      if (forward)
        emitter_.add_immediate(reg, reg, high, true);
      else
        emitter_.subtract_immediate(reg, reg, high, true);
    }
    if (low != 0U) {
      if (forward)
        emitter_.add_immediate(reg, reg, low);
      else
        emitter_.subtract_immediate(reg, reg, low);
    }
  }

bool JitCompiler::emit_run_span_proof(std::uint64_t first, std::uint64_t end,
                                      Arm64Emitter::Label outside) {
    /*
     * The run translates its first accessed byte, so only the bytes it
     * accesses must share a page: a base near a page end whose fields all
     * lie on the next page stays on the native path. The caller moves the
     * translated address back by `first` before the accesses, which keep
     * their own unsigned offsets.
     */
    move_run_base(first, true);
    emit_untag_data_address(8);
    const auto last_delta = end - first - 1U;
    if (last_delta == 0U)
      return false;
    if (last_delta <= 0xfffU) {
      emitter_.add_immediate(0, 8, static_cast<unsigned>(last_delta));
    } else {
      emitter_.load_constant(0, last_delta);
      emitter_.add_shifted(0, 8, 0, 0);
    }
    /*
     * last_delta is translation-time bounded below one guest page. The page
     * bit changes iff the first and last accessed bytes are on different
     * pages.
     */
    emitter_.emit(0xca08'0000U); // eor x0, x0, x8
    emitter_.test_zero(outside, 0, page_shift, true); // tbnz x0, #page_shift
    return true;
  }

namespace {

/* Flag-free cold-path words. */
constexpr std::uint32_t sub_x10_x11_x8 = 0xcb08'016aU;  // sub x10, x11, x8
constexpr std::uint32_t sub_x11_x11_x10 = 0xcb0a'016bU; // sub x11, x11, x10
constexpr std::uint32_t eor_x1_x8_x11 = 0xca0b'0101U;   // eor x1, x8, x11
constexpr std::uint32_t asr_x0_x0_63 = 0x937f'fc00U;    // asr x0, x0, #63
constexpr std::uint32_t and_x0_x0_x1 = 0x8a01'0000U;    // and x0, x0, x1
constexpr std::uint32_t eor_x0_x0_x8 = 0xca08'0000U;    // eor x0, x0, x8

} // namespace

void JitCompiler::emit_run_straddle(const RunStraddle &straddle) {
    emitter_.bind(straddle.label);
    if (straddle.count == 0U ||
        emitter_.patched(straddle.words_begin, straddle.words_end)) {
      emitter_.branch(straddle.slow);
      return;
    }
    const auto subtract = [&](unsigned destination, unsigned source,
                              std::uint64_t value) {
      /* value is at most one page. */
      const auto high = static_cast<unsigned>(value >> 12U);
      const auto low = static_cast<unsigned>(value & 0xfffU);
      if (high != 0U) {
        emitter_.subtract_immediate(destination, source, high, true);
        source = destination;
      }
      if (low != 0U || source != destination)
        emitter_.subtract_immediate(destination, source, low);
    };
    /*
     * x8 holds the guest address of the run's first accessed byte, A. The
     * run's bytes lie in A's page and the next one: x11 = the next page's
     * guest address, and x10 = the bytes from A to it, at least one.
     */
    emitter_.emit(0xb240'0000U | ((page_shift - 1U) << 10U) | (8U << 5U) |
                  11U); // orr x11, x8, #(page_size - 1)
    emitter_.add_immediate(11, 11, 1);
    emitter_.emit(sub_x10_x11_x8);
    /*
     * An access that itself spans the boundary, start < x10 < end, takes
     * the semantic path before any access is made.
     */
    for (std::size_t index = 0; index < straddle.count; ++index) {
      const auto &access = straddle.accesses[index];
      if (access.end - access.start == 1U)
        continue;
      const auto whole = emitter_.make_label();
      subtract(0, 10, access.end);
      emitter_.test_zero(whole, 0, 63, false);  // x10 >= end: in A's page
      subtract(0, 10, access.start);
      emitter_.compare_zero(whole, 0, true, false); // x10 == start
      emitter_.test_zero(straddle.slow, 0, 63, false); // start < x10
      emitter_.bind(whole);
    }
    /*
     * Translate both pages. A miss in the inline TLB takes the semantic
     * path, which fills the TLB.
     */
    const unsigned page_offset =
        straddle.access == Access::write ? 16U : 0U;
    for (const unsigned address : {8U, 11U}) {
      emit_tlb_set(address);
      emitter_.load_pair_x(2, 9, 0, page_offset);
      emitter_.emit(0xca01'0042U); // eor x2, x2, x1
      emitter_.compare_zero(straddle.slow, 2, true, true);
      emitter_.add_shifted(address, address, 9, 0);
    }
    /*
     * x8 becomes the host base for accesses in A's page and x11 the host
     * base for the next page's: the next page's host address less the
     * guest distance from the run's base to it.
     */
    move_run_base(straddle.first, false);
    emitter_.emit(sub_x11_x11_x10);
    move_run_base(straddle.first, false, 11);
    emitter_.emit(eor_x1_x8_x11);
    /*
     * Replay the hot accesses with their binding loads and stores. Each
     * access takes x8 when it ends in A's page, x10 >= end, and x11
     * otherwise, selected without flags into x0.
     */
    std::size_t next = 0;
    for (auto word = straddle.words_begin; word < straddle.words_end;
         ++word) {
      if (next < straddle.count && straddle.accesses[next].word == word) {
        subtract(0, 10, straddle.accesses[next].end);
        emitter_.emit(asr_x0_x0_63);
        emitter_.emit(and_x0_x0_x1);
        emitter_.emit(eor_x0_x0_x8);
        emitter_.emit(emitter_.word(word) & ~(31U << 5U));
        ++next;
        continue;
      }
      emitter_.emit(emitter_.word(word));
    }
    emitter_.branch(straddle.join);
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_sp_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    const auto [operations, length, base, load, first_offset, maximum_end] =
        collect_unsigned_run<maximum_run, UnsignedMemoryOperation>(
            instructions,
            [](std::uint32_t instruction, UnsignedMemoryOperation &operation) {
              return decode_scalar_unsigned_memory(instruction, true,
                                                   operation);
            });
    if (length < 2)
      return 0;

    const auto alignment = cold_exit(operations[0].instruction, pc,
                                     ColdPath::Action::alignment_fault);
    const auto slow = cold_exit(operations[0].instruction, pc,
                                ColdPath::Action::memory_slow);

    load_gpr(8, 31, true);
    /* Every selected instruction names SP, so check its architectural
     * alignment once before observing any memory or destination register. */
    emit_misalignment_check(8, 16, alignment);

    RunStraddle straddle{
        .label = emitter_.make_label(),
        .slow = slow,
        .access = load ? Access::read : Access::write,
        .first = first_offset,
    };
    bool proved = false;
    if (first_offset == 0U) {
      const auto last_unit =
          static_cast<unsigned>((maximum_end - 1U) >> 4U);
      if (last_unit != 0U) {
        /*
         * SP is known to be 16-byte aligned. q+last_unit is below twice the
         * number of units in a page, so its page bit is the complete
         * crossing test.
         */
        emitter_.ubfm(0, 8, 4, page_shift - 1);
        emitter_.add_immediate(0, 0, last_unit);
        emitter_.test_zero(straddle.label, 0, page_shift - 4U, true);
        proved = true;
      }
      emit_untag_data_address(8);
    } else {
      proved = emit_run_span_proof(first_offset, maximum_end, straddle.label);
    }

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    move_run_base(first_offset, false);
    emit_native_gpr_run(std::span{operations}.first(length), load, straddle,
                        proved);

    if (!load)
      mark_direct_memory_writes(length);
    return length;
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_gpr_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    constexpr std::size_t maximum_run = 4;
    if (track_direct_stores_ || instructions.size() < 2)
      return 0;

    const auto [operations, length, base, load, first_offset, maximum_end] =
        collect_unsigned_run<maximum_run, UnsignedMemoryOperation>(
            instructions,
            [](std::uint32_t instruction, UnsignedMemoryOperation &operation) {
              /*
               * A load into Rn changes the architectural base observed by
               * every following instruction. Keeping the translated host
               * base in x8 would therefore be stale. Store Rt==Rn is safe
               * because stores do not change the source register.
               */
              return decode_scalar_unsigned_memory(instruction, false,
                                                   operation) &&
                     !(operation.load && operation.transfer == operation.base);
            });
    if (length < 2)
      return 0;

    /*
     * Record the semantic first-instruction edge before loading or allocating
     * any run register. If the page proof or permission translation fails,
     * the ordinary slow helper executes exactly operation zero with the
     * architectural prefix state and precise fault address.
     */
    const auto slow = cold_exit(operations[0].instruction, pc,
                                ColdPath::Action::memory_slow);

    load_gpr(8, base, true);
    RunStraddle straddle{
        .label = emitter_.make_label(),
        .slow = slow,
        .access = load ? Access::read : Access::write,
        .first = first_offset,
    };
    const bool proved =
        emit_run_span_proof(first_offset, maximum_end, straddle.label);

    emit_translate(8, load ? Access::read : Access::write, slow, slow, 1);
    move_run_base(first_offset, false);
    emit_native_gpr_run(std::span{operations}.first(length), load, straddle,
                        proved);

    if (!load)
      mark_direct_memory_writes(length);
    return length;
  }

[[nodiscard]] bool JitCompiler::emit_unsigned_memory(std::uint32_t instruction,
                                          std::uint64_t pc) {
    const bool vector = (instruction & 0x0400'0000U) != 0;
    if (vector && !host_capabilities().floating_point)
      return false;
    const unsigned size_shift = instruction >> 30;
    const unsigned opc = (instruction >> 22) & 3U;
    /*
     * PRFM is an architecturally non-faulting hint.  Its Rt field is a
     * prefetch operation, not a destination register.  Treating opc=2
     * uniformly as a signed load corrupts that register and can also
     * introduce a spurious memory fault.
     */
    if (!vector && size_shift == 3U && opc == 2U)
      return true;
    if (vector && opc >= 2U && size_shift != 0U)
      return false;
    const std::size_t bytes =
        vector && opc >= 2U ? 16U : std::size_t{1} << size_shift;
    if (!vector && ((bytes == 8U && opc >= 2U) || (bytes == 4U && opc == 3U)))
      return false;
    const unsigned base = (instruction >> 5) & 31U;
    const unsigned transfer = instruction & 31U;
    const auto offset =
        static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) * bytes;
    /* opc==0 is the only scalar store allocation. Signed loads use
     * opc==2/3, so testing only the ordinary-load bit would silently
     * discard every LDRSB/LDRSH/LDRSW result.
     */
    const bool load = vector ? (opc & 1U) != 0 : opc != 0;
    if (!load)
      mark_direct_memory_write();
    load_gpr(8, base, true);
    if (base == 31U) {
      const auto alignment =
          cold_exit(instruction, pc, ColdPath::Action::alignment_fault);
      emit_misalignment_check(8, 16, alignment);
    }
    if (offset != 0) {
      emitter_.load_constant(9, offset);
      emitter_.add_shifted(8, 8, 9, 0);
    }
    const auto fallback =
        cold_exit(instruction, pc, ColdPath::Action::memory_slow);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    emit_untag_data_address(8);
    emit_translate(8, load ? Access::read : Access::write, fallback, fault,
                   bytes);
    if (!load)
      track_direct_store(8, bytes);
    const auto native_vector =
        vector
            ? (load ? write_native_vector(transfer, false, 0)
                    : read_native_vector(transfer, 0))
            : NativeVector{};
    const auto native_gpr =
        !vector
            ? (load ? write_native_gpr(transfer, false, false, 9)
                    : read_native_gpr(transfer, false, 9))
            : NativeGpr{};
    const auto native = (instruction & ~((0xfffU << 10) | (31U << 5) | 31U)) |
                        (8U << 5) |
                        (vector ? native_vector.host : native_gpr.host);
    emitter_.emit(native);
    if (load) {
      if (vector)
        commit_native_vector(native_vector, transfer);
      else
        commit_native_gpr(native_gpr, transfer, false);
    }
    return true;
  }

} // namespace uml::cpu::arm64
