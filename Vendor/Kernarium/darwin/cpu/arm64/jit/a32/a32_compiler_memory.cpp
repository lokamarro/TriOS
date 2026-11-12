// SPDX-License-Identifier: GPL-2.0-or-later
//
// Memory: the inline lowering of the transfer instructions through the
// shared TLB fast path, and the cold edges to the slow helper.
//
// An address is a W value zero-extended into x8, which the translation
// replaces in place by the host pointer. A page crossing, a misalignment the
// architecture faults on and every exclusive take the cold path to
// uml_arm64_jit_a32_execute, which runs the whole instruction; the hot path
// therefore changes no architectural state before it knows the access
// completes. The translation clobbers x0, x1, x2, x9 and, on a miss, every
// other scratch register, so a register the access needs afterwards is read
// again from its resident host or CpuState (a writeback recomputes its
// value for that reason).

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_helpers.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

#include "cpu/arm64/memory.hpp"
#include "memory/tlb.hpp"

#include <bit>

namespace uml::cpu::arm64::jit::a32 {

using ::uml::memory::Access;
using aarch32::SRType;

namespace {

constexpr unsigned writeback_m = 1;

} // namespace

// ---------------------------------------------------------------------------
// The shared TLB fast path (a copy of JitCompiler::emit_translate; the A64
// compiler's is a member of a class this one is not part of).

void A32Compiler::emit_tlb_index(unsigned address) {
  emitter_.ubfm(1, address, page_shift, 63U); // lsr x1, Xn, #page_shift
  emitter_.ubfm(0, address, page_shift,
                page_shift + ::uml::memory::tlb_index_bits - 1U);
  emitter_.add_shifted(0, 14, 0, 5);
}

void A32Compiler::emit_translate(unsigned address, Access access,
                                 Arm64Emitter::Label fallback,
                                 Arm64Emitter::Label fault,
                                 std::size_t bytes,
                                 std::optional<Arm64Emitter::Label> crossing,
                                 unsigned aligned, bool fold) {
  const auto crossing_target = crossing.value_or(fallback);
  /* The register whose page the entry's tag is compared with. */
  [[maybe_unused]] unsigned tag_address = address;
  bool tag_checks_crossing = false;
  if (aligned >= bytes)
    crossing.reset();
  else if (bytes > 1) {
    /* The entry is indexed by the first byte's page and compared with the
     * last byte's: an access inside one page hits as before, and a crossing
     * one finds an entry of another page (the direct index never gives two
     * neighbouring pages the same entry), misses, and is told from a plain
     * miss on the cold path. */
    emitter_.add_immediate(1, address, static_cast<unsigned>(bytes - 1));
    tag_address = 1;
    tag_checks_crossing = true;
  }
  const unsigned page_offset = access == Access::write ? 16U : 0U;
  const auto slow = emitter_.make_label();
  const auto complete = emitter_.make_label();
  /* The index is the page number's low bits, extracted from the address, and
   * the tag compare shifts the address itself: one instruction fewer than
   * emit_tlb_index and a compare. */
  emitter_.ubfm(0, address, page_shift,
                page_shift + ::uml::memory::tlb_index_bits - 1U);
  emitter_.add_shifted(0, 14, 0, 5);
  emitter_.load_pair_x(2, 9, 0, page_offset);
  emitter_.emit(0xca40'0042U | (page_shift << 10) |
                ((tag_address & 31U) << 16)); // eor x2, x2, tag, lsr #12
  emitter_.compare_zero(slow, 2, true, true);
  /* A folded access leaves the address a guest one, and the delta in x9. */
  if (!fold)
    emitter_.add_shifted(address, address, 9, 0);
  emitter_.bind(complete);
  translation_cold_.push_back({
      .label = slow,
      .continuation = complete,
      .fault = fault,
      .address = address,
      .access = access,
      .bytes = tag_checks_crossing ? bytes : 0U,
      .crossing = crossing_target,
      .fold = fold,
  });
  if (crossing && bytes > 1 && aligned < bytes)
    crossing_cold_.push_back({
        .label = *crossing,
        .continuation = complete,
        .slow = fallback,
        .address = address,
        .bytes = bytes,
        .fold = fold,
    });
}

/* The load crossed into the next page. Both pages' read translations come
 * from the TLB (a miss on either leaves the instruction to the library, which
 * fills it). When the two mappings are adjacent in the host a single access
 * serves, as within one page. Otherwise the last 32 bytes of the first page
 * and the first 32 of the second are copied side by side into the staging
 * area and the load reads from there, at the guest's offset. Only x0-x2, x9,
 * x10, x16, V0-V3 and x8 are used, and the host flags are not. */
void A32Compiler::emit_crossing_cold(const CrossingColdPath &path) {
  constexpr unsigned first_delta = 16;
  constexpr unsigned last_address = 10;
  emitter_.bind(path.label);
  const auto probe = [&](unsigned address) {
    emit_tlb_index(address);
    emitter_.load_pair_x(2, 9, 0, 0);
    emitter_.emit(0xca01'0042U); // eor x2, x2, x1
    emitter_.compare_zero(path.slow, 2, true, true);
  };
  probe(path.address);
  emitter_.move_x(first_delta, 9);
  emitter_.add_immediate(last_address, path.address,
                         static_cast<unsigned>(path.bytes - 1U));
  probe(last_address);
  const auto staged = emitter_.make_label();
  emitter_.emit(0xca09'0000U | first_delta << 5); // eor x0, x16, x9
  emitter_.compare_zero(staged, 0, true, true);
  if (!path.fold)
    emitter_.add_shifted(path.address, path.address, 9, 0);
  emitter_.branch(path.continuation);
  emitter_.bind(staged);
  // x0: the second page's first byte; x8 - x0 is the first page's share.
  emitter_.emit(0xb240'0000U | (page_shift - 1U) << 10 |
                path.address << 5); // orr x0, x8, #(page - 1)
  emitter_.add_immediate(0, 0, 1);
  emitter_.add_shifted(1, 0, first_delta, 0);
  emitter_.subtract_immediate(1, 1, 32);
  emitter_.add_shifted(2, 0, 9, 0);
  emit(enc::vec::ldp_q(0, 1, 1, 0));
  emit(enc::vec::ldp_q(2, 3, 2, 0));
  emitter_.add_immediate(1, 15, offsetof(CpuState, a32_cross));
  emit(enc::vec::stp_q(0, 1, 1, 0));
  emit(enc::vec::stp_q(2, 3, 1, 32));
  emitter_.emit(0xcb00'0000U | path.address << 16); // sub x0, x0, x8
  emitter_.add_immediate(path.address, 1, 32);
  emitter_.emit(0xcb00'0000U | path.address << 5 |
                path.address); // sub x8, x8, x0
  if (path.fold) {
    emitter_.move_x(9, path.address);
    emitter_.move_x(path.address, 31);
  }
  emitter_.branch(path.continuation);
}

void A32Compiler::emit_translation_cold(const TranslationColdPath &path) {
  emitter_.bind(path.label);
  if (path.bytes != 0U) {
    /* The tag compared with the last byte's page: a crossing access ends up
     * here too, and leaves for the crossing or slow path without a walk. */
    emitter_.ubfm(0, path.address, 0, page_shift - 1);
    emitter_.add_immediate(0, 0, static_cast<unsigned>(path.bytes - 1));
    emitter_.test_zero(path.crossing, 0, page_shift, true);
  }
  emitter_.move_x(0, 14);
  emitter_.move_x(1, path.address);
  emitter_.load_constant(2, path.access == Access::write ? 1U : 0U);
  emitter_.call_preserving_flags(
      reinterpret_cast<const void *>(&uml_arm64_jit_tlb_translate_preserving));
  if (path.fold) {
    /* The host pointer is the whole of the address: x9 + (w8 = 0). */
    emitter_.compare_zero(path.fault, 0, true, false);
    emitter_.move_x(9, 0);
    emitter_.move_x(path.address, 31);
  } else {
    emitter_.move_x(path.address, 0);
    emitter_.compare_zero(path.fault, path.address, true, false);
  }
  emitter_.branch(path.continuation);
}

void A32Compiler::emit_alignment_check(unsigned address, unsigned alignment,
                                       Arm64Emitter::Label slow) {
  if (alignment <= 1U)
    return;
  emitter_.ubfm(0, address, 0,
                static_cast<unsigned>(std::countr_zero(alignment) - 1U));
  emitter_.compare_zero(slow, 0, true, true);
}

// ---------------------------------------------------------------------------
// Store bookkeeping (copies of the A64 compiler's).

void A32Compiler::mark_memory_write() noexcept {
  if (count_ < maximum_block_instructions)
    memory_write_mask_ |= std::uint64_t{1} << count_;
}

void A32Compiler::mark_direct_memory_write() noexcept {
  mark_memory_write();
  if (count_ < maximum_block_instructions)
    direct_memory_write_mask_ |= std::uint64_t{1} << count_;
}

/* Records the range a tracked block stores to, for the software reservation
 * table. `address` is the permission-checked host pointer. */
void A32Compiler::track_direct_store(unsigned address, std::size_t size) {
  if (!track_direct_stores_ || count_ >= maximum_block_instructions)
    return;
  const auto offset = count_ * sizeof(JitStoreRange);
  emitter_.load_x(3, 31, 32);
  emitter_.load_x(3, 3, offsetof(JitChainState, stores));
  emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
  emitter_.load_constant(4, size);
  emitter_.store_x(4, 3, offset + offsetof(JitStoreRange, size));
}

/* Ends the software monitor where the guest's pair ends: a CLREX, the
 * store-exclusive of a fused run and its exits. An ordinary store of the
 * guest's own leaves it standing (the monitor contract in
 * docs/architecture.md). */
void A32Compiler::clear_exclusive_monitor() {
  emitter_.store_b(31, 15, offsetof(CpuState, exclusive_size));
}

// ---------------------------------------------------------------------------
// The dispatcher.

A32Compiler::Flow A32Compiler::emit_memory(const OpInfo &) {
  const auto transfer = describe_transfer(cur().inst);
  switch (transfer.kind) {
  case Transfer::Kind::single: return emit_single(transfer);
  case Transfer::Kind::pair: return emit_pair(transfer);
  case Transfer::Kind::multiple: return emit_multiple(transfer);
  case Transfer::Kind::table: return emit_table(transfer);
  case Transfer::Kind::exclusive: return emit_exclusive(transfer);
  case Transfer::Kind::prefetch: return Flow::next;
  case Transfer::Kind::none: break;
  }
  exit_illegal();
  return Flow::end;
}

/* The instruction in the slow helper, which retires it and ends the block. */
A32Compiler::Flow A32Compiler::emit_through_helper() {
  const auto transfer = describe_transfer(cur().inst);
  if (!transfer.load)
    mark_memory_write();
  if (transfer.kind == Transfer::Kind::exclusive) {
    mark_memory_write();
    emitter_.branch(cold_exit(ColdPath::Kind::exclusive_slow));
  } else {
    emitter_.branch(cold_exit(ColdPath::Kind::slow));
  }
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Addresses.

/* destination = base plus or minus the register offset m, shifted as the
 * transfer says. */
void A32Compiler::emit_register_offset(unsigned destination, unsigned base,
                                       const Transfer &transfer, unsigned m) {
  enc::Shift fold = enc::lsl;
  unsigned amount = 0;
  unsigned operand = m;
  switch (transfer.shift_type) {
  case SRType::LSL:
    amount = transfer.shift_amount;
    break;
  case SRType::LSR:
    if (transfer.shift_amount < 32U) {
      fold = enc::lsr;
      amount = transfer.shift_amount;
    } else {
      operand = shift_value(m, SRType::LSR, transfer.shift_amount, temp0);
    }
    break;
  case SRType::ASR:
    fold = enc::asr;
    amount = transfer.shift_amount >= 32U ? 31U : transfer.shift_amount;
    break;
  default:
    operand = shift_value(m, transfer.shift_type, transfer.shift_amount, temp0);
    break;
  }
  emit(transfer.add ? enc::add(destination, base, operand, fold, amount)
                    : enc::sub(destination, base, operand, fold, amount));
}

void A32Compiler::form_address(const Transfer &transfer) {
  if (transfer.literal) {
    const std::uint32_t base = pc_align_read();
    load_u32(address_register,
             transfer.add ? base + transfer.imm : base - transfer.imm);
    return;
  }
  const unsigned n = use(transfer.n, fallback_n);
  if (!transfer.index || (!transfer.register_offset && transfer.imm == 0U)) {
    if (n != address_register)
      emit(enc::mov(address_register, n));
    return;
  }
  if (!transfer.register_offset) {
    emit(transfer.add ? enc::add_i(address_register, n, transfer.imm)
                      : enc::sub_i(address_register, n, transfer.imm));
    return;
  }
  emit_register_offset(address_register, n, transfer,
                       use(transfer.m, fallback_m));
}

/* The base register's new value: base plus or minus the offset, computed
 * from the registers as they stand now (a load's destination is never the
 * offset register: that case takes the helper). */
void A32Compiler::write_back(const Transfer &transfer) {
  const auto dest = def(transfer.n, true, fallback_m);
  const unsigned host = dest.native.host;
  if (!transfer.register_offset) {
    if (transfer.imm != 0U)
      emit(transfer.add ? enc::add_i(host, host, transfer.imm)
                        : enc::sub_i(host, host, transfer.imm));
  } else {
    emit_register_offset(host, host, transfer,
                         use(transfer.m, writeback_m));
  }
  done(dest);
}

// ---------------------------------------------------------------------------
// Single transfers.

A32Compiler::Flow A32Compiler::emit_single(const Transfer &transfer) {
  /* A load whose destination is also the offset register of a writeback form
   * would feed the loaded value to the recomputation of the base, so it loads
   * into a scratch register, recomputes the base from the registers as they
   * stand, and then writes the destination. The base as the destination or
   * the PC anywhere is unpredictable: the helper runs those. The PC as a
   * store's offset register is excluded by the decoder. */
  const bool over_offset = transfer.wback && transfer.register_offset &&
                           transfer.load && transfer.t == transfer.m;
  if (over_offset && (transfer.n == transfer.t || transfer.n == 15U ||
                      transfer.t == 15U))
    return emit_through_helper();
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  form_address(transfer);
  /* A load-acquire and a store-release fault unless aligned to the size; the
   * helper reports it. */
  if (transfer.ordered)
    emit_alignment_check(address_register, transfer.bytes, slow);
  /* The access adds the translation's delta itself, [x9, w8, uxtw], where a
   * form exists: not for the acquire and release forms, nor for a store that
   * records the host pointer or whose value is read into x9. */
  const bool fold =
      !transfer.ordered &&
      !(store && (transfer.t == 15U ||
                  (track_direct_stores_ &&
                   count_ < maximum_block_instructions)));
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, transfer.bytes, std::nullopt,
                 transfer.ordered ? static_cast<unsigned>(transfer.bytes) : 0U,
                 fold);
  const unsigned size_field = transfer.bytes == 1U ? 0U
                              : transfer.bytes == 2U ? 1U
                                                     : 2U;
  /* The base of the access: x8 holding the host pointer, or x9 indexed by
   * w8. */
  const unsigned base = fold ? 9U : address_register;
  const auto indexed = [&](std::uint32_t transfer_word) {
    return fold ? enc::indexed_uxtw(transfer_word, address_register)
                : transfer_word;
  };
  if (store) {
    track_direct_store(address_register, transfer.bytes);
    const unsigned value =
        transfer.t == 15U ? (load_u32(fallback_n, pc_read()), fallback_n)
                          : use(transfer.t, fold ? fallback_m : fallback_n);
    if (transfer.ordered) {
      emit(enc::stlr(size_field, value, address_register));
    } else {
      switch (transfer.bytes) {
      case 1: emit(indexed(enc::strb(value, base, 0))); break;
      case 2: emit(indexed(enc::strh(value, base, 0))); break;
      default: emit(indexed(enc::str_w(value, base, 0))); break;
      }
    }
    if (transfer.wback)
      write_back(transfer);
    return Flow::next;
  }
  const bool writes_pc = transfer.t == 15U;
  Dest dest{};
  unsigned target = fallback_n;
  if (!writes_pc && !over_offset) {
    dest = def(transfer.t, false, fallback_n);
    target = dest.native.host;
  }
  if (transfer.ordered) {
    emit(enc::ldar(size_field, target, address_register));
  } else {
    switch (transfer.bytes) {
    case 1:
      emit(indexed(transfer.sign_extend ? enc::ldrsb(target, base, 0)
                                        : enc::ldrb(target, base, 0)));
      break;
    case 2:
      emit(indexed(transfer.sign_extend ? enc::ldrsh(target, base, 0)
                                        : enc::ldrh(target, base, 0)));
      break;
    default:
      emit(indexed(enc::ldr_w(target, base, 0)));
      break;
    }
  }
  if (over_offset) {
    write_back(transfer);
    dest = def(transfer.t, false, fallback_n);
    if (dest.native.host != fallback_n)
      emit(enc::mov(dest.native.host, fallback_n));
    done(dest);
    return Flow::next;
  }
  if (!writes_pc)
    done(dest);
  if (transfer.wback)
    write_back(transfer);
  if (writes_pc) {
    write_pc(target, true);
    return Flow::end;
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// LDRD and STRD.

A32Compiler::Flow A32Compiler::emit_pair(const Transfer &transfer) {
  if (transfer.wback && transfer.register_offset && transfer.load &&
      (transfer.t == transfer.m || transfer.t2 == transfer.m))
    return emit_through_helper();
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  form_address(transfer);
  /* MemA: a doubleword transfer faults unless word aligned; the helper
   * reports it. */
  emit_alignment_check(address_register, 4, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, 8);
  if (store) {
    track_direct_store(address_register, 8);
    const unsigned first = use(transfer.t, fallback_n);
    const unsigned second = use(transfer.t2, fallback_m);
    emit(enc::x::orr(temp0, first, second, enc::lsl, 32));
    emit(enc::str_x(temp0, address_register, 0));
  } else {
    emit(enc::ldr_x(fallback_m, address_register, 0));
    const auto low = def(transfer.t, false, fallback_n);
    emit(enc::mov(low.native.host, fallback_m));
    done(low);
    const auto high = def(transfer.t2, false, fallback_n);
    emit(enc::x::lsr_i(high.native.host, fallback_m, 32));
    done(high);
  }
  if (transfer.wback)
    write_back(transfer);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// LDM, STM, PUSH and POP.

A32Compiler::Flow A32Compiler::emit_multiple(const Transfer &transfer) {
  const unsigned count = std::popcount(static_cast<unsigned>(transfer.registers));
  const unsigned span = 4U * count;
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const unsigned n = use(transfer.n, fallback_n);
  switch (transfer.mode) {
  case Transfer::Mode::ia:
    if (n != address_register)
      emit(enc::mov(address_register, n));
    break;
  case Transfer::Mode::ib:
    emit(enc::add_i(address_register, n, 4));
    break;
  case Transfer::Mode::da:
    emit(enc::sub_i(address_register, n, span - 4U));
    break;
  case Transfer::Mode::db:
    emit(enc::sub_i(address_register, n, span));
    break;
  }
  emit_alignment_check(address_register, 4, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, span);
  if (store)
    track_direct_store(address_register, span);
  unsigned offset = 0;
  bool writes_pc = false;
  for (unsigned r = 0; r < 16U; ++r) {
    if ((transfer.registers >> r & 1U) == 0U)
      continue;
    if (store) {
      const unsigned value =
          r == 15U ? (load_u32(fallback_n, pc_read()), fallback_n)
                   : use(r, fallback_n);
      emit(enc::str_w(value, address_register, offset));
    } else if (r == 15U) {
      emit(enc::ldr_w(fallback_n, address_register, offset));
      writes_pc = true;
    } else {
      const auto dest = def(r, false, fallback_n);
      emit(enc::ldr_w(dest.native.host, address_register, offset));
      done(dest);
    }
    offset += 4U;
  }
  if (transfer.wback) {
    const auto dest = def(transfer.n, true, fallback_m);
    const unsigned host = dest.native.host;
    const bool up = transfer.mode == Transfer::Mode::ia ||
                    transfer.mode == Transfer::Mode::ib;
    emit(up ? enc::add_i(host, host, span) : enc::sub_i(host, host, span));
    done(dest);
  }
  if (writes_pc) {
    write_pc(fallback_n, true);
    return Flow::end;
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// TBB and TBH.

A32Compiler::Flow A32Compiler::emit_table(const Transfer &transfer) {
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const unsigned n = use(transfer.n, fallback_n);
  const unsigned m = use(transfer.m, fallback_m);
  emit(enc::add(address_register, n, m, enc::lsl, transfer.halfword_table ? 1U : 0U));
  emit_translate(address_register, Access::read, slow, fault, transfer.bytes);
  emit(transfer.halfword_table ? enc::ldrh(fallback_n, address_register, 0)
                               : enc::ldrb(fallback_n, address_register, 0));
  emit(enc::lsl_i(fallback_n, fallback_n, 1));
  load_u32(fallback_m, pc_read());
  emit(enc::add(fallback_n, fallback_n, fallback_m));
  build_thumb_key(fallback_n);
  exit_indirect(address_register);
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Exclusives.

A32Compiler::Flow A32Compiler::emit_exclusive(const Transfer &) {
  return emit_through_helper();
}

} // namespace uml::cpu::arm64::jit::a32
