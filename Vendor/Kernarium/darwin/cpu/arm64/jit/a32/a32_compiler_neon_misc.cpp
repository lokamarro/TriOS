// SPDX-License-Identifier: GPL-2.0-or-later
//
// Advanced SIMD: the two-register operations, the conversions, the modified
// immediates and the data-movement instructions (duplicate, extract, table
// lookup, swap, transpose, zip and unzip).

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr unsigned s_table = 2; // V2 and V3
constexpr unsigned s_mask = 7;
constexpr unsigned fallback_t = 9;

} // namespace

// ---------------------------------------------------------------------------
// Two registers.

A32Compiler::Flow A32Compiler::emit_neon_misc(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  const unsigned size = esize_log2(o.esize);
  using enc::vec::FloatMisc;
  using enc::vec::Misc;

  switch (v.mnemonic) {
  case VM::vabs:
  case VM::vneg:
    if (o.floating_point) {
      neon_op2(enc::vec::float_misc(quad, v.mnemonic == VM::vabs ? enc::vec::fabs_
                                                                 : enc::vec::fneg,
                                    false, 0, 0),
               quad, false, d, m, false);
    } else {
      neon_op2(enc::vec::misc(quad, v.mnemonic == VM::vabs ? enc::vec::abs_
                                                           : enc::vec::neg,
                              size, 0, 0),
               quad, false, d, m, false);
    }
    return Flow::next;
  case VM::vqabs:
  case VM::vqneg:
    touch_fp_environment();
    neon_op2(enc::vec::misc(quad, v.mnemonic == VM::vqabs ? enc::vec::sqabs
                                                          : enc::vec::sqneg,
                            size, 0, 0),
             quad, false, d, m, false);
    return Flow::next;
  case VM::vcls:
    neon_op2(enc::vec::misc(quad, enc::vec::cls, size, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  case VM::vclz:
    neon_op2(enc::vec::misc(quad, enc::vec::clz, size, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  case VM::vcnt:
    neon_op2(enc::vec::misc(quad, enc::vec::cnt, 0, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  case VM::vrev16:
    neon_op2(enc::vec::misc(quad, enc::vec::rev16, size, 0, 0), quad, false, d,
             m, false);
    return Flow::next;
  case VM::vrev32:
    neon_op2(enc::vec::misc(quad, enc::vec::rev32, size, 0, 0), quad, false, d,
             m, false);
    return Flow::next;
  case VM::vrev64:
    neon_op2(enc::vec::misc(quad, enc::vec::rev64, size, 0, 0), quad, false, d,
             m, false);
    return Flow::next;
  case VM::vpaddl:
    neon_op2(enc::vec::misc(quad, o.unsigned_ ? enc::vec::uaddlp : enc::vec::saddlp,
                            size, 0, 0),
             quad, false, d, m, false);
    return Flow::next;
  case VM::vpadal:
    neon_op2(enc::vec::misc(quad, o.unsigned_ ? enc::vec::uadalp : enc::vec::sadalp,
                            size, 0, 0),
             quad, false, d, m, true);
    return Flow::next;
  case VM::vrecpe:
  case VM::vrsqrte: {
    const bool recip = v.mnemonic == VM::vrecpe;
    if (o.floating_point) {
      touch_fp_environment();
      set_fp_mode(true);
      neon_op2(enc::vec::float_misc(quad, recip ? enc::vec::frecpe
                                                : enc::vec::frsqrte,
                                    false, 0, 0),
               quad, false, d, m, false);
    } else {
      neon_op2(enc::vec::misc(quad, recip ? enc::vec::urecpe : enc::vec::ursqrte,
                              2, 0, 0),
               quad, false, d, m, false);
    }
    return Flow::next;
  }
  case VM::vceq: case VM::vcge: case VM::vcgt: case VM::vcle: case VM::vclt: {
    // Comparison with zero.
    if (o.floating_point) {
      touch_fp_environment();
      set_fp_mode(true);
      FloatMisc op = enc::vec::fcmeq0;
      switch (v.mnemonic) {
      case VM::vceq: op = enc::vec::fcmeq0; break;
      case VM::vcge: op = enc::vec::fcmge0; break;
      case VM::vcgt: op = enc::vec::fcmgt0; break;
      case VM::vcle: op = enc::vec::fcmle0; break;
      default: op = enc::vec::fcmlt0; break;
      }
      neon_op2(enc::vec::float_misc(quad, op, false, 0, 0), quad, false, d, m,
               false);
    } else {
      Misc op = enc::vec::cmeq0;
      switch (v.mnemonic) {
      case VM::vceq: op = enc::vec::cmeq0; break;
      case VM::vcge: op = enc::vec::cmge0; break;
      case VM::vcgt: op = enc::vec::cmgt0; break;
      case VM::vcle: op = enc::vec::cmle0; break;
      default: op = enc::vec::cmlt0; break;
      }
      neon_op2(enc::vec::misc(quad, op, size, 0, 0), quad, false, d, m, false);
    }
    return Flow::next;
  }
  case VM::vmovn:
  case VM::vqmovn:
  case VM::vqmovun: {
    // Narrowing from the Q register m>>1 into D register d.
    Misc op = enc::vec::xtn;
    if (v.mnemonic == VM::vqmovun) {
      op = enc::vec::sqxtun;
    } else if (v.mnemonic == VM::vqmovn) {
      op = o.src_unsigned ? enc::vec::uqxtn : enc::vec::sqxtn;
    }
    if (v.mnemonic != VM::vmovn)
      touch_fp_environment();
    const unsigned source = q_use(m >> 1, s_m);
    emit_vec2(enc::vec::misc(false, op, size, 0, 0), s_result, source);
    d_write(d, s_result);
    return Flow::next;
  }
  case VM::vcvt:
    return emit_neon_convert(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// VCVT between floating point and integer or fixed point, and between single
// and half precision, on the Standard FPSCR value.

A32Compiler::Flow A32Compiler::emit_neon_convert(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  touch_fp_environment();
  set_fp_mode(true);
  switch (v.variant) {
  case Variant::is: {
    using enc::vec::FloatMisc;
    const FloatMisc op = o.to_integer
                             ? (o.unsigned_ ? enc::vec::fcvtzu : enc::vec::fcvtzs)
                             : (o.unsigned_ ? enc::vec::ucvtf : enc::vec::scvtf);
    neon_op2(enc::vec::float_misc(quad, op, false, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  }
  case Variant::xs: {
    using enc::vec::ShiftImm;
    const ShiftImm op = o.to_fixed
                            ? (o.unsigned_ ? enc::vec::fcvtzu_i : enc::vec::fcvtzs_i)
                            : (o.unsigned_ ? enc::vec::ucvtf_i : enc::vec::scvtf_i);
    neon_op2(enc::vec::shift_imm(quad, op,
                                 64U - static_cast<unsigned>(o.frac_bits), 0, 0),
             quad, false, d, m, false);
    return Flow::next;
  }
  case Variant::sh:
  case Variant::hs: {
    if (o.half_to_single) {
      // Four half-precision lanes of D register m widen into Q register d>>1.
      const unsigned source = d_use(m, s_m);
      const auto dest = q_def(d >> 1, false, s_result);
      emit_vec2(enc::vec::fcvtl(false, false, 0, 0), dest.host, source);
      q_done(dest, d >> 1);
    } else {
      const unsigned source = q_use(m >> 1, s_m);
      emit_vec2(enc::vec::fcvtn(false, false, 0, 0), s_result, source);
      d_write(d, s_result);
    }
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Modified immediates: VMOV, VMVN, VORR and VBIC. The A32 and A64 encodings
// share op, cmode and the eight immediate bits.

A32Compiler::Flow A32Compiler::emit_neon_immediate(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const bool quad = v.quad;
  const std::uint32_t word = neon_word();
  const unsigned op = (word >> 5) & 1U;
  const unsigned cmode = (word >> 8) & 15U;
  const unsigned imm8 = ((word >> 17) & 0x80U) | ((word >> 12) & 0x70U) |
                        (word & 15U);
  const bool accumulate = v.mnemonic == VM::vorr || v.mnemonic == VM::vbic;
  const auto base = enc::vec::modified_immediate(quad, op, cmode, imm8, 0);
  if (quad) {
    const auto dest = q_def(d >> 1, accumulate, s_result);
    emit(base | dest.host);
    q_done(dest, d >> 1);
    return Flow::next;
  }
  if (accumulate)
    vmov(s_result, d_use(d, s_acc));
  emit(base | s_result);
  d_write(d, s_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Duplicate, extract, swap, transpose, zip, unzip and table lookup.

A32Compiler::Flow A32Compiler::emit_neon_permute(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad || o.quadword_operation;
  const unsigned size = esize_log2(o.esize);

  switch (v.mnemonic) {
  case VM::vdup: {
    if (v.variant == Variant::r) {
      const unsigned source = use(static_cast<unsigned>(o.t), fallback_t);
      const std::uint32_t base = enc::vec::dup_general(quad, size, 0, source);
      if (quad) {
        const auto dest = q_def(d >> 1, false, s_result);
        emit(base | dest.host);
        q_done(dest, d >> 1);
      } else {
        emit(base | s_result);
        d_write(d, s_result);
      }
      return Flow::next;
    }
    // From a scalar: the element is lane index_value of D register m.
    const unsigned lanes = 8U >> size;
    const unsigned index =
        (m & 1U) * lanes + static_cast<unsigned>(o.index_value);
    const unsigned source = q_use(m >> 1, s_m);
    if (quad) {
      const auto dest = q_def(d >> 1, false, s_result);
      emit(enc::vec::dup_element(true, size, dest.host, source, index));
      q_done(dest, d >> 1);
    } else {
      emit(enc::vec::dup_element(false, size, s_result, source, index));
      d_write(d, s_result);
    }
    return Flow::next;
  }
  case VM::vext:
    // `position` is the extraction point in bits.
    neon_op3(enc::vec::ext(quad, 0, 0, 0, static_cast<unsigned>(o.position) / 8U),
             quad, false, d, static_cast<unsigned>(o.n), m, false);
    return Flow::next;
  case VM::vswp:
  case VM::vtrn:
  case VM::vzip:
  case VM::vuzp: {
    if (v.mnemonic == VM::vtrn && d == m && size == 2U) {
      // The reference implementation exchanges the 32-bit elements of the
      // one register, which reverses each D register.
      neon_op2(enc::vec::misc(quad, enc::vec::rev64, size, 0, 0), quad, false,
               d, m, false);
      return Flow::next;
    }
    // Both registers are read before either is written.
    unsigned a, b;
    if (quad) {
      a = q_use(d >> 1, s_n);
      b = q_use(m >> 1, s_m);
    } else {
      a = d_use(d, s_n);
      if (a != s_n)
        vmov(s_n, a);
      a = s_n;
      b = d_use(m, s_m);
      if (b != s_m)
        vmov(s_m, b);
      b = s_m;
    }
    if (v.mnemonic == VM::vswp) {
      if (quad) {
        // The first write may overwrite the host register of `a`.
        vmov(s_n, a);
        vmov(s_extra, b);
        q_put(d >> 1, s_extra);
        q_put(m >> 1, s_n);
      } else {
        d_write(d, b);
        d_write(m, a);
      }
      return Flow::next;
    }
    using enc::vec::Permute;
    Permute first = enc::vec::trn1, second = enc::vec::trn2;
    if (v.mnemonic == VM::vzip) {
      first = enc::vec::zip1;
      second = enc::vec::zip2;
    } else if (v.mnemonic == VM::vuzp) {
      first = enc::vec::uzp1;
      second = enc::vec::uzp2;
    }
    emit_vec3(enc::vec::permute(quad, first, size, 0, 0, 0), s_result, a, b);
    emit_vec3(enc::vec::permute(quad, second, size, 0, 0, 0), s_extra, a, b);
    // With d == m the result is UNKNOWN; the second register is written
    // first, as the reference implementation does.
    if (quad) {
      q_put(m >> 1, s_extra);
      q_put(d >> 1, s_result);
    } else {
      d_write(m, s_extra);
      d_write(d, s_result);
    }
    return Flow::next;
  }
  case VM::vtbl:
  case VM::vtbx:
    return emit_neon_table(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

/* VTBL and VTBX: a table of 1-4 D registers starting at n, looked up with
 * the byte indices of D register m. The table goes to consecutive scratch
 * registers, 16 bytes each; the half of the last one that no D register fills
 * is zero, which a VTBL cannot tell from an index out of range, and a VTBX
 * restores the destination for explicitly. */
A32Compiler::Flow A32Compiler::emit_neon_table(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const unsigned length = static_cast<unsigned>(o.length);
  const bool extension = v.mnemonic == VM::vtbx;
  const unsigned registers = (length + 1U) / 2U;
  const bool padded = (length & 1U) != 0U;

  for (unsigned k = 0; k < registers; ++k) {
    const unsigned first = n + 2U * k;
    const unsigned target = s_table + k;
    if (2U * k + 1U < length) {
      if ((first & 1U) == 0U) {
        vmov(target, q_use(first >> 1, target));
      } else {
        // D(first) is the high half of one Q register and D(first+1) the low
        // half of the next.
        const unsigned low = q_use(first >> 1, s_extra);
        const unsigned high = q_use((first + 1U) >> 1, s_mask);
        emit(enc::vec::ext(true, target, low, high, 8));
      }
    } else {
      // A lone D register, with the upper half zero.
      const unsigned source = d_use(first, s_extra);
      emit(enc::vec::bitwise_op(false, enc::vec::orr, target, source, source));
    }
  }
  const unsigned index = d_use(m, s_m);
  if (extension && padded) {
    // Out-of-range lanes keep the destination, including those the padding
    // would otherwise answer with zero.
    const unsigned old = d_use(d, s_acc == s_table ? s_n : s_acc);
    vmov(s_extra, old);
    emit(enc::vec::table(false, false, registers, s_result, s_table, index));
    emit(enc::vec::modified_immediate(false, 0, 14, 8U * length, s_mask));
    emit_vec3(enc::vec::same(false, enc::vec::cmhs, 0, 0, 0, 0), s_mask, index,
              s_mask);
    emit_vec3(enc::vec::bitwise_op(false, enc::vec::bsl, 0, 0, 0), s_mask,
              s_extra, s_result);
    d_write(d, s_mask);
    return Flow::next;
  }
  if (extension)
    vmov(s_result, d_use(d, s_n));
  emit(enc::vec::table(false, extension, registers, s_result, s_table, index));
  d_write(d, s_result);
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
