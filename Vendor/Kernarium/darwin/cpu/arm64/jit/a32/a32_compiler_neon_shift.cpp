// SPDX-License-Identifier: GPL-2.0-or-later
//
// Advanced SIMD: the shifts by an immediate, the operations that widen or
// narrow (three registers of different widths, narrowing shifts, VMOVL and
// VSHLL) and the operations by scalar.
//
// A64 encodes a shift by immh:immb, which is the A32 imm6 for the narrowing
// and widening shifts and L:imm6 for the others, so each is built here from
// the decoded element size and shift amount.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

namespace uml::cpu::arm64::jit::a32 {

// ---------------------------------------------------------------------------
// Shifts by an immediate on registers of one width.

A32Compiler::Flow A32Compiler::emit_neon_shift(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  // The register width is the instruction's Q bit.
  const bool quad = ((neon_word() >> 6) & 1U) != 0U;
  const unsigned esize = static_cast<unsigned>(o.esize);
  const unsigned shift = static_cast<unsigned>(o.shift_amount);
  const bool wide64 = !quad && esize == 64U;
  using enc::vec::ShiftImm;

  // The narrowing and widening shifts have no D/Q suffix.
  switch (v.mnemonic) {
  case VM::vshrn: case VM::vrshrn: case VM::vqshrn: case VM::vqrshrn:
  case VM::vqshrun: case VM::vqrshrun: {
    ShiftImm op = enc::vec::shrn;
    bool saturating = true;
    switch (v.mnemonic) {
    case VM::vshrn: op = enc::vec::shrn; saturating = false; break;
    case VM::vrshrn: op = enc::vec::rshrn; saturating = false; break;
    case VM::vqshrn:
      op = o.src_unsigned ? enc::vec::uqshrn : enc::vec::sqshrn;
      break;
    case VM::vqrshrn:
      op = o.src_unsigned ? enc::vec::uqrshrn : enc::vec::sqrshrn;
      break;
    case VM::vqshrun: op = enc::vec::sqshrun; break;
    default: op = enc::vec::sqrshrun; break;
    }
    if (saturating)
      touch_fp_environment();
    const unsigned source = q_use(m >> 1, s_m);
    emit_vec2(enc::vec::shift_imm(false, op, 2U * esize - shift, 0, 0),
              s_result, source);
    d_write(d, s_result);
    return Flow::next;
  }
  case VM::vshll:
  case VM::vmovl: {
    const unsigned left = v.mnemonic == VM::vmovl ? 0U : shift;
    const unsigned source = d_use(m, s_m);
    const auto dest = q_def(d >> 1, false, s_result);
    if (left == esize) {
      emit_vec2(enc::vec::misc(false, enc::vec::shll, esize_log2(o.esize), 0, 0),
                dest.host, source);
    } else {
      emit_vec2(enc::vec::shift_imm(false,
                                    o.unsigned_ ? enc::vec::ushll : enc::vec::sshll,
                                    esize + left, 0, 0),
                dest.host, source);
    }
    q_done(dest, d >> 1);
    return Flow::next;
  }
  default:
    break;
  }

  ShiftImm op = enc::vec::sshr;
  bool left = false;
  bool accumulate = false;
  bool saturating = false;
  const bool u = o.unsigned_;
  switch (v.mnemonic) {
  case VM::vshr: op = u ? enc::vec::ushr : enc::vec::sshr; break;
  case VM::vsra: op = u ? enc::vec::usra : enc::vec::ssra; accumulate = true; break;
  case VM::vrshr: op = u ? enc::vec::urshr : enc::vec::srshr; break;
  case VM::vrsra: op = u ? enc::vec::ursra : enc::vec::srsra; accumulate = true; break;
  case VM::vshl: op = enc::vec::shl; left = true; break;
  case VM::vsli: op = enc::vec::sli; left = true; accumulate = true; break;
  case VM::vsri: op = enc::vec::sri; accumulate = true; break;
  case VM::vqshl:
    op = o.src_unsigned ? enc::vec::uqshl_i : enc::vec::sqshl_i;
    left = true;
    saturating = true;
    break;
  case VM::vqshlu: op = enc::vec::sqshlu; left = true; saturating = true; break;
  default:
    exit_illegal();
    return Flow::end;
  }
  if (saturating)
    touch_fp_environment();
  const unsigned immhb = left ? esize + shift : 2U * esize - shift;
  neon_op2(enc::vec::shift_imm(quad || wide64, op, immhb, 0, 0), quad, wide64,
           d, m, accumulate);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Three registers of different widths: long, wide and narrowing operations.

A32Compiler::Flow A32Compiler::emit_neon_diff(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const unsigned size = esize_log2(o.esize);
  const bool u = o.unsigned_;
  using enc::vec::Diff;

  Diff op = enc::vec::saddl;
  bool wide_first = false;   // the first source is a Q register (VADDW)
  bool narrowing = false;    // the result is a D register (VADDHN)
  bool accumulate = false;
  bool saturating = false;
  switch (v.mnemonic) {
  case VM::vaddl: op = u ? enc::vec::uaddl : enc::vec::saddl; break;
  case VM::vaddw: op = u ? enc::vec::uaddw : enc::vec::saddw; wide_first = true; break;
  case VM::vsubl: op = u ? enc::vec::usubl : enc::vec::ssubl; break;
  case VM::vsubw: op = u ? enc::vec::usubw : enc::vec::ssubw; wide_first = true; break;
  case VM::vabal: op = u ? enc::vec::uabal : enc::vec::sabal; accumulate = true; break;
  case VM::vabdl: op = u ? enc::vec::uabdl : enc::vec::sabdl; break;
  case VM::vmlal: op = u ? enc::vec::umlal : enc::vec::smlal; accumulate = true; break;
  case VM::vmlsl: op = u ? enc::vec::umlsl : enc::vec::smlsl; accumulate = true; break;
  case VM::vmull:
    op = o.polynomial ? enc::vec::pmull : (u ? enc::vec::umull : enc::vec::smull);
    break;
  case VM::vqdmlal: op = enc::vec::sqdmlal; accumulate = true; saturating = true; break;
  case VM::vqdmlsl: op = enc::vec::sqdmlsl; accumulate = true; saturating = true; break;
  case VM::vqdmull: op = enc::vec::sqdmull; saturating = true; break;
  case VM::vaddhn: op = enc::vec::addhn; narrowing = true; break;
  case VM::vsubhn: op = enc::vec::subhn; narrowing = true; break;
  case VM::vraddhn: op = enc::vec::raddhn; narrowing = true; break;
  case VM::vrsubhn: op = enc::vec::rsubhn; narrowing = true; break;
  default:
    exit_illegal();
    return Flow::end;
  }
  if (saturating)
    touch_fp_environment();
  const std::uint32_t base = enc::vec::diff(false, op, size, 0, 0, 0);
  if (narrowing) {
    const unsigned a = q_use(n >> 1, s_n);
    const unsigned b = q_use(m >> 1, s_m);
    emit_vec3(base, s_result, a, b);
    d_write(d, s_result);
    return Flow::next;
  }
  const unsigned a = wide_first ? q_use(n >> 1, s_n) : d_use(n, s_n);
  const unsigned b = d_use(m, s_m);
  const auto dest = q_def(d >> 1, accumulate, s_result);
  emit_vec3(base, dest.host, a, b);
  q_done(dest, d >> 1);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Operations by scalar: the second operand is one element of a D register.

A32Compiler::Flow A32Compiler::emit_neon_scalar(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  const unsigned size = esize_log2(o.esize);
  const unsigned lanes = 64U / static_cast<unsigned>(o.esize);
  const unsigned index = (m & 1U) * lanes + static_cast<unsigned>(o.index_value);
  using enc::vec::Element;

  const bool is_float = o.floating_point;
  const bool long_form = v.mnemonic == VM::vmlal || v.mnemonic == VM::vmlsl ||
                         v.mnemonic == VM::vmull || v.mnemonic == VM::vqdmlal ||
                         v.mnemonic == VM::vqdmlsl || v.mnemonic == VM::vqdmull;
  bool accumulate = false;
  bool saturating = false;
  Element op = enc::vec::elem_mul;
  switch (v.mnemonic) {
  case VM::vmul: op = is_float ? enc::vec::elem_fmul : enc::vec::elem_mul; break;
  case VM::vmla: op = enc::vec::elem_mla; accumulate = true; break;
  case VM::vmls: op = enc::vec::elem_mls; accumulate = true; break;
  case VM::vmlal:
    op = o.unsigned_ ? enc::vec::elem_umlal : enc::vec::elem_smlal;
    accumulate = true;
    break;
  case VM::vmlsl:
    op = o.unsigned_ ? enc::vec::elem_umlsl : enc::vec::elem_smlsl;
    accumulate = true;
    break;
  case VM::vmull: op = o.unsigned_ ? enc::vec::elem_umull : enc::vec::elem_smull; break;
  case VM::vqdmlal: op = enc::vec::elem_sqdmlal; accumulate = true; saturating = true; break;
  case VM::vqdmlsl: op = enc::vec::elem_sqdmlsl; accumulate = true; saturating = true; break;
  case VM::vqdmull: op = enc::vec::elem_sqdmull; saturating = true; break;
  case VM::vqdmulh: op = enc::vec::elem_sqdmulh; saturating = true; break;
  case VM::vqrdmulh: op = enc::vec::elem_sqrdmulh; saturating = true; break;
  default:
    exit_illegal();
    return Flow::end;
  }
  if (saturating)
    touch_fp_environment();

  // The scalar's Q register; a halfword element must be in V0-V15.
  unsigned scalar = q_use(m >> 1, s_m);
  if (size == 1U && scalar >= 16U) {
    vmov(s_m, scalar);
    scalar = s_m;
  }

  if (is_float) {
    // VMUL by scalar is a plain FMUL by element; VMLA and VMLS round the
    // product first.
    touch_fp_environment();
    set_fp_mode(true);
    const unsigned a = vsrc(n, quad, s_n);
    if (v.mnemonic == VM::vmul) {
      emit(enc::vec::float_element(quad, enc::vec::elem_fmul, false, s_result, a,
                                   scalar, index));
    } else {
      emit(enc::vec::float_element(quad, enc::vec::elem_fmul, false, s_extra, a,
                                   scalar, index));
      const unsigned old = vsrc(d, quad, s_acc);
      emit_vec3(enc::vec::float_same(quad,
                                     v.mnemonic == VM::vmla ? enc::vec::fadd
                                                            : enc::vec::fsub,
                                     false, 0, 0, 0),
                s_result, old, s_extra);
    }
    neon_put(d, quad, s_result);
    return Flow::next;
  }

  if (long_form) {
    const unsigned a = d_use(n, s_n);
    const auto dest = q_def(d >> 1, accumulate, s_result);
    emit(enc::vec::element(false, op, size, dest.host, a, scalar, index));
    q_done(dest, d >> 1);
    return Flow::next;
  }
  if (quad) {
    const unsigned a = q_use(n >> 1, s_n);
    const auto dest = q_def(d >> 1, accumulate, s_result);
    emit(enc::vec::element(true, op, size, dest.host, a, scalar, index));
    q_done(dest, d >> 1);
    return Flow::next;
  }
  const unsigned a = d_use(n, s_n);
  if (accumulate)
    vmov(s_result, d_use(d, s_acc));
  emit(enc::vec::element(false, op, size, s_result, a, scalar, index));
  d_write(d, s_result);
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
