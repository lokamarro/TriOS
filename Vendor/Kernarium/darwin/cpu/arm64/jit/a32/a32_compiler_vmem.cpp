// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFP and Advanced SIMD memory: VLDR and VSTR, the register-list transfers
// (VLDM, VSTM, FLDMX, FSTMX) and the structure loads and stores VLD1-4 and
// VST1-4.
//
// The lowering is the scalar one's (a32_compiler_memory.cpp): the address in
// x8, an alignment check where the architecture faults, the shared TLB fast
// path over the whole span, and the data movement only after the translation
// is known to succeed. A page crossing, a misalignment the architecture
// faults on and the forms below that the lowering declines all take the cold
// path to uml_arm64_jit_a32_execute, which runs the instruction in the
// semantic library and so faults, and orders its effects, as the reference
// does; the hot path changes no architectural state before the access is
// known to complete. A load of at most 32 bytes that crosses a page stays in
// the translation (emit_crossing_cold) and takes the library only when
// either page is missing from the TLB.
//
// A run of consecutive registers moves as whole Q registers where it is
// aligned to one (memory order is the little-endian register order, so a Q
// is one 16-byte access) and as 32-bit or 64-bit lanes elsewhere. The
// interleaving structure transfers use the host LDn and STn on the scratch
// registers V0-V3 and move the result to or from the register halves.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_helpers.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

#include "memory/tlb.hpp"

#include <algorithm>
#include <array>

namespace uml::cpu::arm64::jit::a32 {

using ::uml::memory::Access;

namespace {

constexpr unsigned s_data = 0;
constexpr unsigned s_view = 6;

/* The most a load can span and still cross a page through the staging area
 * (CpuState::a32_cross holds 32 bytes from each page). */
constexpr std::size_t staged_load_limit = 32;

/* The log2 of the element size that selects the LD1/ST1 lane forms (2 = 32
 * bits, 3 = 64 bits). */
constexpr unsigned word_size = 2;
constexpr unsigned double_size = 3;

} // namespace

// ---------------------------------------------------------------------------
// Register granules.

/* The S (log2_size 2) or D (3) register `index`, loaded from or stored to the
 * host memory at x8 + offset. */
void A32Compiler::vmem_granule(bool load, unsigned log2_size, unsigned index,
                               unsigned offset) {
  const unsigned per_q = 16U >> log2_size;
  const unsigned q = index / per_q;
  const unsigned lane = index % per_q;
  if (load) {
    const auto dest = q_def(q, true, 2);
    if (offset == 0U) {
      emit(enc::vec::structure_lane(true, 1, log2_size, lane, dest.host,
                                    address_register));
    } else {
      emit(enc::vec::ldur_simd(log2_size, s_data, address_register,
                               static_cast<int>(offset)));
      emit(enc::vec::ins_element(log2_size, dest.host, lane, s_data, 0));
    }
    q_done(dest, q);
    return;
  }
  const unsigned host = q_use(q, s_view);
  if (offset == 0U) {
    emit(enc::vec::structure_lane(false, 1, log2_size, lane, host,
                                  address_register));
    return;
  }
  unsigned source = host;
  if (lane != 0U) {
    emit(enc::vec::dup_scalar(log2_size, s_data, host, lane));
    source = s_data;
  }
  emit(enc::vec::stur_simd(log2_size, source, address_register,
                           static_cast<int>(offset)));
}

/* Whether a transfer of `bytes` starting at register `first` (of `per_q` to
 * a Q register) is one whole Q register, which the access can address as
 * [x9, w8, uxtw]: not a store that records its host pointer. */
bool A32Compiler::vmem_folds(bool store, unsigned bytes, unsigned first,
                             unsigned per_q) const {
  return bytes == 16U && first % per_q == 0U &&
         !(store && track_direct_stores_ &&
           count_ < maximum_block_instructions);
}

/* `count` consecutive S or D registers from `first`, loaded from or stored to
 * the memory at x8 in register order (or at x9 indexed by w8 for one Q
 * register, when `fold`). */
void A32Compiler::vmem_run(bool load, bool single, unsigned first,
                           unsigned count, bool fold) {
  const unsigned log2_size = single ? word_size : double_size;
  const unsigned unit = 1U << log2_size;
  const unsigned per_q = 16U / unit;
  unsigned index = first;
  unsigned moved = 0;
  while (moved < count) {
    const unsigned remaining = count - moved;
    const unsigned offset = moved * unit;
    if (index % per_q == 0U && remaining >= per_q) {
      const unsigned q = index / per_q;
      /* A run of one register adds the translation's delta in the access
       * itself, [x9, w8, uxtw]. */
      const unsigned base = fold ? 9U : address_register;
      const auto indexed = [&](std::uint32_t word) {
        return fold ? enc::vec::indexed_uxtw(word, address_register) : word;
      };
      if (load) {
        const auto dest = q_def(q, false, 2);
        emit(indexed(enc::vec::ldur_simd(4, dest.host, base,
                                         static_cast<int>(offset))));
        q_done(dest, q);
      } else {
        emit(indexed(enc::vec::stur_simd(4, q_use(q, s_view), base,
                                         static_cast<int>(offset))));
      }
      index += per_q;
      moved += per_q;
      continue;
    }
    if (single && index % 2U == 0U && remaining >= 2U) {
      vmem_granule(load, double_size, index / 2U, offset);
      index += 2U;
      moved += 2U;
      continue;
    }
    vmem_granule(load, log2_size, index, offset);
    ++index;
    ++moved;
  }
}

/* The doubleword D(dreg) takes half `half` of `source`. */
void A32Compiler::vmem_d_from(unsigned dreg, unsigned source, unsigned half) {
  const auto dest = q_def(dreg >> 1, true, 2);
  emit(enc::vec::ins_element(3, dest.host, dreg & 1U, source, half));
  q_done(dest, dreg >> 1);
}

/* Half `half` of the scratch register `scratch` takes the doubleword
 * D(dreg). */
void A32Compiler::vmem_d_into(unsigned dreg, unsigned scratch, unsigned half) {
  emit(enc::vec::ins_element(3, scratch, half, q_use(dreg >> 1, s_view),
                             dreg & 1U));
}

A32Compiler::Flow A32Compiler::vmem_through_helper(bool store) {
  if (store)
    mark_memory_write();
  emitter_.branch(cold_exit(ColdPath::Kind::slow));
  return Flow::end;
}

// ---------------------------------------------------------------------------
// The dispatcher.

A32Compiler::Flow A32Compiler::emit_vector_memory(const VInfo &v) {
  switch (v.mnemonic) {
  case VM::vldr:
  case VM::vstr:
    return emit_vector_register_transfer(v.mnemonic == VM::vstr);
  case VM::vldm: case VM::vldmdb: case VM::vstm: case VM::vstmdb:
  case VM::fldmiax: case VM::fldmdbx: case VM::fstmiax: case VM::fstmdbx:
    return emit_vector_block_transfer(vector_store(v.mnemonic));
  case VM::vld1: case VM::vld2: case VM::vld3: case VM::vld4:
  case VM::vst1: case VM::vst2: case VM::vst3: case VM::vst4:
    return emit_vector_structure(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// VLDR and VSTR.

A32Compiler::Flow A32Compiler::emit_vector_register_transfer(bool store) {
  const auto &o = cur().inst.operands;
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned bytes = static_cast<unsigned>(o.esize) / 8U;
  const unsigned reg = static_cast<unsigned>(o.d);
  // A store from the PC base is UNPREDICTABLE, left to the library.
  if (bytes != 4U && bytes != 8U)
    return vmem_through_helper(store);
  if (store && n == 15U)
    return vmem_through_helper(store);
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const auto offset = static_cast<std::uint32_t>(o.imm32);
  if (n == 15U) {
    const std::uint32_t base = pc_align_read();
    load_u32(address_register, o.add ? base + offset : base - offset);
  } else {
    const unsigned base = use(n, fallback_n);
    if (offset == 0U) {
      if (base != address_register)
        emit(enc::mov(address_register, base));
    } else {
      emit(o.add ? enc::add_i(address_register, base, offset)
                 : enc::sub_i(address_register, base, offset));
    }
  }
  // MemA: both widths need the alignment of their words.
  emit_alignment_check(address_register, 4, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, bytes,
                 store ? std::nullopt : std::optional{make_label()}, 4U);
  const unsigned log2_size = bytes == 4U ? word_size : double_size;
  if (store) {
    track_direct_store(address_register, bytes);
    vmem_granule(false, log2_size, reg, 0);
  } else if (s_shadowing_ && bytes == 4U) {
    /* VLDR of one S register (begin_s_shadows): the word goes to a shadow,
     * after the translation's cold paths, and from there to its lane. */
    const unsigned shadow = take_s_shadow(reg);
    emit(enc::vec::ldur_simd(word_size, shadow, address_register, 0));
    const auto dest = q_def(reg >> 2, true, s_view);
    emit(enc::vec::ins_element(word_size, dest.host, reg & 3U, shadow, 0));
    q_done(dest, reg >> 2);
  } else {
    vmem_granule(true, log2_size, reg, 0);
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VLDM, VSTM, FLDMX and FSTMX.

A32Compiler::Flow A32Compiler::emit_vector_block_transfer(bool store) {
  const auto &o = cur().inst.operands;
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned regs = static_cast<unsigned>(o.regs);
  const unsigned first = static_cast<unsigned>(o.d);
  const bool single = o.single_regs;
  // An empty or oversized list, one past the register file, and the PC as
  // the base are UNPREDICTABLE: the library decides.
  if (n == 15U || regs == 0U || regs > (single ? 32U : 16U) ||
      first + regs > 32U)
    return vmem_through_helper(store);
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const auto offset = static_cast<std::uint32_t>(o.imm32);
  const unsigned bytes = (single ? 4U : 8U) * regs;
  const unsigned base = use(n, fallback_n);
  if (o.add) {
    if (base != address_register)
      emit(enc::mov(address_register, base));
  } else {
    emit(enc::sub_i(address_register, base, offset));
  }
  emit_alignment_check(address_register, 4, slow);
  const bool fold = vmem_folds(store, bytes, first, single ? 4U : 2U);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, bytes,
                 store || bytes > staged_load_limit
                     ? std::nullopt
                     : std::optional{make_label()},
                 4U, fold);
  if (store)
    track_direct_store(address_register, bytes);
  vmem_run(!store, single, first, regs, fold);
  if (o.wback) {
    const auto dest = def(n, true, fallback_m);
    emit(o.add ? enc::add_i(dest.native.host, dest.native.host, offset)
               : enc::sub_i(dest.native.host, dest.native.host, offset));
    done(dest);
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VLD1-4 and VST1-4.

A32Compiler::Flow A32Compiler::emit_vector_structure(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const bool store = vector_store(v.mnemonic);
  const unsigned structs =
      v.mnemonic == VM::vld1 || v.mnemonic == VM::vst1   ? 1U
      : v.mnemonic == VM::vld2 || v.mnemonic == VM::vst2 ? 2U
      : v.mnemonic == VM::vld3 || v.mnemonic == VM::vst3 ? 3U
                                                         : 4U;
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned ebytes = static_cast<unsigned>(o.ebytes);
  const unsigned size = esize_log2(static_cast<int>(8U * ebytes));
  const std::array<unsigned, 4> dregs{
      static_cast<unsigned>(o.d), static_cast<unsigned>(o.d2),
      static_cast<unsigned>(o.d3), static_cast<unsigned>(o.d4)};
  const bool multiple = v.variant == Variant::m;
  const bool lane = v.variant == Variant::one;
  const bool all = v.variant == Variant::a;
  if (n == 15U || (!multiple && !lane && !all) || ebytes == 0U ||
      ebytes > 8U)
    return vmem_through_helper(store);

  // The transfer's size and the registers it names; one past the register
  // file is UNPREDICTABLE and left to the library.
  unsigned bytes = 0;
  unsigned highest = 0;
  unsigned run = 0;
  if (multiple) {
    if (structs == 1U) {
      run = static_cast<unsigned>(o.regs);
      bytes = 8U * run;
      highest = dregs[0] + run - 1U;
    } else if (structs == 2U) {
      const unsigned pairs = static_cast<unsigned>(o.pairs);
      bytes = 16U * pairs;
      highest = std::max(dregs[0], dregs[1]) + pairs - 1U;
    } else {
      bytes = 8U * structs;
      highest = std::max(std::max(dregs[0], dregs[1]),
                         std::max(dregs[2], dregs[structs - 1U]));
    }
  } else {
    bytes = structs * ebytes;
    for (unsigned s = 0; s < structs; ++s)
      highest = std::max(highest, dregs[s]);
    if (all && structs == 1U) {
      run = static_cast<unsigned>(o.regs);
      highest = dregs[0] + run - 1U;
    }
  }
  if (bytes == 0U || highest > 31U || (multiple && structs == 1U && run > 4U) ||
      (multiple && structs == 2U && o.pairs > 2) ||
      (all && structs == 1U && run != 1U && run != 2U) ||
      (lane && static_cast<unsigned>(o.index_value) * ebytes >= 8U))
    return vmem_through_helper(store);

  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const unsigned base = use(n, fallback_n);
  if (base != address_register)
    emit(enc::mov(address_register, base));
  if (o.alignment > 1)
    emit_alignment_check(address_register,
                         static_cast<unsigned>(o.alignment), slow);
  const bool fold = multiple && structs == 1U &&
                    vmem_folds(store, bytes, dregs[0], 2U);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, bytes,
                 store || bytes > staged_load_limit
                     ? std::nullopt
                     : std::optional{make_label()},
                 o.alignment > 1 ? static_cast<unsigned>(o.alignment) : 0U,
                 fold);
  if (store)
    track_direct_store(address_register, bytes);

  using enc::vec::Structure;
  if (multiple && structs == 1U) {
    vmem_run(!store, false, dregs[0], run, fold);
  } else if (multiple) {
    const Structure op = structs == 2U   ? enc::vec::ld2
                         : structs == 3U ? enc::vec::ld3
                                         : enc::vec::ld4;
    const bool wide = structs == 2U && o.pairs == 2;
    if (store) {
      if (wide) {
        vmem_d_into(dregs[0], s_data, 0);
        vmem_d_into(dregs[0] + 1U, s_data, 1);
        vmem_d_into(dregs[1], s_data + 1U, 0);
        vmem_d_into(dregs[1] + 1U, s_data + 1U, 1);
      } else {
        for (unsigned s = 0; s < structs; ++s) {
          const unsigned host = q_use(dregs[s] >> 1, s_view);
          if ((dregs[s] & 1U) == 0U)
            vmov(s_data + s, host);
          else
            emit(enc::vec::dup_scalar(3, s_data + s, host, 1));
        }
      }
      emit(enc::vec::structure_multiple(wide, false, op, size, s_data,
                                        address_register));
    } else {
      emit(enc::vec::structure_multiple(wide, true, op, size, s_data,
                                        address_register));
      if (wide) {
        vmem_d_from(dregs[0], s_data, 0);
        vmem_d_from(dregs[0] + 1U, s_data, 1);
        vmem_d_from(dregs[1], s_data + 1U, 0);
        vmem_d_from(dregs[1] + 1U, s_data + 1U, 1);
      } else {
        for (unsigned s = 0; s < structs; ++s)
          vmem_d_from(dregs[s], s_data + s, 0);
      }
    }
  } else if (lane) {
    const unsigned per_d = 8U / ebytes;
    for (unsigned s = 0; s < structs; ++s) {
      const unsigned index =
          (dregs[s] & 1U) * per_d + static_cast<unsigned>(o.index_value);
      const unsigned q = dregs[s] >> 1;
      if (store) {
        emit(enc::vec::structure_lane(false, 1, size, index,
                                      q_use(q, s_view), address_register));
      } else {
        const auto dest = q_def(q, true, 2);
        emit(enc::vec::structure_lane(true, 1, size, index, dest.host,
                                      address_register));
        q_done(dest, q);
      }
      if (s + 1U < structs)
        emitter_.add_immediate(address_register, address_register, ebytes);
    }
  } else {
    // Replicate to all lanes: only loads exist.
    if (structs == 1U) {
      emit(enc::vec::structure_replicate(run == 2U, 1, size, s_data,
                                         address_register));
      for (unsigned r = 0; r < run; ++r)
        vmem_d_from(dregs[0] + r, s_data, 0);
    } else {
      emit(enc::vec::structure_replicate(false, structs, size, s_data,
                                         address_register));
      for (unsigned s = 0; s < structs; ++s)
        vmem_d_from(dregs[s], s_data + s, 0);
    }
  }

  if (o.wback) {
    const auto dest = def(n, true, fallback_m);
    if (o.register_index) {
      const unsigned m = use(static_cast<unsigned>(o.m), fallback_n);
      emit(enc::add(dest.native.host, dest.native.host, m));
    } else {
      emit(enc::add_i(dest.native.host, dest.native.host, bytes));
    }
    done(dest);
  }
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
