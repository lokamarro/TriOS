// SPDX-License-Identifier: GPL-2.0-or-later
//
// The integer instructions beside data processing: multiplies and divides,
// bit-field and byte-order operations, extension, packing and SEL,
// saturation, and the DSP multiplies. All arithmetic is on W registers; the
// 64-bit forms use the host's long multiplies. The parallel add and subtract
// forms run in the vector unit, except the signed saturating ones, which with
// SSAT16, USAT16 and the sum of absolute differences run in the AArch32
// semantic library, through the slow helper (a32_execute.cpp).
//
// Registers: an operand lives in its resident register or a fallback (x9 for
// Rn, x10 for Rm, x2 for Ra or RdLo, x8 for RdHi), x0, x1, x2, x8 and x16
// are temporaries and x11 holds the NZCV image.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

namespace uml::cpu::arm64::jit::a32 {

using aarch32::SRType;

namespace {

constexpr unsigned fallback_a = 2;
constexpr unsigned fallback_hi = 8;
constexpr unsigned temp3 = 16;
constexpr unsigned image = 11;

[[nodiscard]] std::uint32_t field(std::uint32_t value) noexcept {
  return *enc::logical_immediate_field(value);
}

} // namespace

// ---------------------------------------------------------------------------
// Multiplies and divides.

A32Compiler::Flow A32Compiler::emit_multiply(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  const Op op = info.op;
  switch (op) {
  case Op::mul:
  case Op::mla:
  case Op::mls: {
    const bool set = o.setflags;
    /* MULS and MLAS set N and Z and keep C and V. */
    if (set)
      read_flags(image);
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    const unsigned a = op == Op::mul ? 31U : use(o.a, fallback_a);
    const auto dest = def(o.d, false, fallback_n);
    const unsigned r = dest.native.host;
    emit(op == Op::mls ? enc::msub(r, n, m, a) : enc::madd(r, n, m, a));
    if (set) {
      emit(enc::tst(r, r));
      restore_logical_flags(CarryKind::unchanged, 0, 0);
      store_flags();
    }
    done(dest);
    return Flow::next;
  }
  case Op::sdiv:
  case Op::udiv: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    const auto dest = def(o.d, false, fallback_n);
    emit(op == Op::sdiv ? enc::sdiv(dest.native.host, n, m)
                        : enc::udiv(dest.native.host, n, m));
    done(dest);
    return Flow::next;
  }
  case Op::umull:
  case Op::smull:
  case Op::umlal:
  case Op::smlal:
  case Op::umaal: {
    const bool set = o.setflags && op != Op::umaal;
    const bool is_signed = op == Op::smull || op == Op::smlal;
    if (set)
      read_flags(image);
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    if (op == Op::umull || op == Op::smull) {
      emit(is_signed ? enc::smull(temp0, n, m) : enc::umull(temp0, n, m));
    } else {
      const unsigned lo = use(o.dLo, fallback_a);
      const unsigned hi = use(o.dHi, fallback_hi);
      if (op == Op::umaal) {
        emit(enc::umaddl(temp0, n, m, lo));
        emit(enc::x::add(temp0, temp0, hi));
      } else {
        emit(enc::x::orr(temp3, lo, hi, enc::lsl, 32));
        emit(is_signed ? enc::smaddl(temp0, n, m, temp3)
                       : enc::umaddl(temp0, n, m, temp3));
      }
    }
    if (set) {
      emit(enc::x::tst(temp0, temp0));
      restore_logical_flags(CarryKind::unchanged, 0, 0);
      store_flags();
    }
    const auto lo = def(o.dLo, false, fallback_n);
    const auto hi = def(o.dHi, false, fallback_m);
    emit(enc::mov(lo.native.host, temp0));
    emit(enc::x::lsr_i(hi.native.host, temp0, 32));
    done(lo);
    done(hi);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Bit fields, counts and byte order.

A32Compiler::Flow A32Compiler::emit_bit_operation(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  switch (info.op) {
  case Op::clz:
  case Op::rbit:
  case Op::rev:
  case Op::rev16:
  case Op::revsh: {
    const unsigned m = use(o.m, fallback_m);
    const auto dest = def(o.d, false, fallback_n);
    const unsigned r = dest.native.host;
    switch (info.op) {
    case Op::clz: emit(enc::clz(r, m)); break;
    case Op::rbit: emit(enc::rbit(r, m)); break;
    case Op::rev: emit(enc::rev(r, m)); break;
    case Op::rev16: emit(enc::rev16(r, m)); break;
    default:
      emit(enc::rev16(temp0, m));
      emit(enc::sxth(r, temp0));
      break;
    }
    done(dest);
    return Flow::next;
  }
  case Op::bfc:
  case Op::bfi: {
    const unsigned lsb = static_cast<unsigned>(o.lsbit);
    const unsigned width = static_cast<unsigned>(o.msbit) - lsb + 1U;
    const unsigned n = info.op == Op::bfc ? 31U : use(o.n, fallback_m);
    const auto dest = def(o.d, true, fallback_n);
    emit(enc::bfi(dest.native.host, n, lsb, width));
    done(dest);
    return Flow::next;
  }
  case Op::sbfx:
  case Op::ubfx: {
    const unsigned lsb = static_cast<unsigned>(o.lsbit);
    const unsigned width = static_cast<unsigned>(o.widthminus1) + 1U;
    const unsigned n = use(o.n, fallback_m);
    const auto dest = def(o.d, false, fallback_n);
    emit(info.op == Op::sbfx ? enc::sbfx(dest.native.host, n, lsb, width)
                             : enc::ubfx(dest.native.host, n, lsb, width));
    done(dest);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Extension, packing and SEL.

/* out = the zero- or sign-extended byte or halfword of `source` rotated right
 * by `rotation` (0, 8, 16 or 24). */
void A32Compiler::emit_extract(unsigned out, unsigned source, bool is_signed,
                               unsigned bits, unsigned rotation) {
  if (rotation + bits <= 32U) {
    emit(is_signed ? enc::sbfx(out, source, rotation, bits)
                   : enc::ubfx(out, source, rotation, bits));
    return;
  }
  emit(enc::ror_i(out, source, rotation));
  if (bits == 8U)
    emit(is_signed ? enc::sxtb(out, out) : enc::uxtb(out, out));
  else
    emit(is_signed ? enc::sxth(out, out) : enc::uxth(out, out));
}

A32Compiler::Flow A32Compiler::emit_extend(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  const unsigned rotation = static_cast<unsigned>(o.rotation);
  switch (info.op) {
  case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth: {
    const bool is_signed = info.op == Op::sxtb || info.op == Op::sxth;
    const unsigned bits = info.op == Op::sxtb || info.op == Op::uxtb ? 8U : 16U;
    const unsigned m = use(o.m, fallback_m);
    const auto dest = def(o.d, false, fallback_n);
    emit_extract(dest.native.host, m, is_signed, bits, rotation);
    done(dest);
    return Flow::next;
  }
  case Op::sxtab: case Op::sxtah: case Op::uxtab: case Op::uxtah: {
    const bool is_signed = info.op == Op::sxtab || info.op == Op::sxtah;
    const unsigned bits = info.op == Op::sxtab || info.op == Op::uxtab ? 8U : 16U;
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit_extract(temp0, m, is_signed, bits, rotation);
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::add(dest.native.host, n, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::sxtb16: case Op::uxtb16: case Op::sxtab16: case Op::uxtab16: {
    const bool is_signed = info.op == Op::sxtb16 || info.op == Op::sxtab16;
    const bool accumulate = info.op == Op::sxtab16 || info.op == Op::uxtab16;
    const unsigned m = use(o.m, fallback_m);
    unsigned n = 31;
    if (accumulate)
      n = use(o.n, fallback_n);
    // x0 = the rotated operand; the two extended bytes land in x0.
    unsigned source = m;
    if (rotation != 0U) {
      emit(enc::ror_i(temp0, m, rotation));
      source = temp0;
    }
    if (is_signed) {
      emit(enc::sxtb(temp1, source));
      emit(enc::uxth(temp1, temp1));
      emit(enc::sbfx(temp2, source, 16, 8));
      emit(enc::lsl_i(temp2, temp2, 16));
      emit(enc::orr(temp0, temp1, temp2));
    } else {
      emit(enc::and_i(temp0, source, field(0x00ff'00ffU)));
    }
    const auto dest = def(o.d, false, fallback_n);
    if (!accumulate) {
      emit(enc::mov(dest.native.host, temp0));
    } else {
      // Per-halfword addition without a carry between the halves.
      emit(enc::and_i(temp1, n, field(0x7fff'7fffU)));
      emit(enc::and_i(temp2, temp0, field(0x7fff'7fffU)));
      emit(enc::add(temp1, temp1, temp2));
      emit(enc::eor(temp2, n, temp0));
      emit(enc::and_i(temp2, temp2, field(0x8000'8000U)));
      emit(enc::eor(dest.native.host, temp1, temp2));
    }
    done(dest);
    return Flow::next;
  }
  case Op::pkh: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    const unsigned amount = static_cast<unsigned>(o.shift_n);
    if (!o.tbform) {
      // PKHBT: the top half of Rm shifted left, the bottom half of Rn.
      if (amount == 0U)
        emit(enc::mov(temp0, m));
      else
        emit(enc::lsl_i(temp0, m, amount));
      emit(enc::bfxil(temp0, n, 0, 16));
    } else {
      // PKHTB: the top half of Rn, the bottom half of Rm shifted right.
      emit(enc::asr_i(temp0, m, amount >= 32U ? 31U : amount));
      emit(enc::mov(temp1, n));
      emit(enc::bfxil(temp1, temp0, 0, 16));
      emit(enc::mov(temp0, temp1));
    }
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::sel: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit(enc::ldr_w(temp0, 15, offsetof(CpuState, a32_ge_mask)));
    emit(enc::and_(temp1, n, temp0));
    emit(enc::bic(temp2, m, temp0));
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::orr(dest.native.host, temp1, temp2));
    done(dest);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Saturation and the Q flag.

/* a32_q |= register (a 0 or 1 in a W register). Clobbers x1. */
void A32Compiler::or_q(unsigned flag) {
  emit(enc::ldrb(temp1, 15, offsetof(CpuState, a32_q)));
  emit(enc::orr(temp1, temp1, flag));
  emit(enc::strb(temp1, 15, offsetof(CpuState, a32_q)));
}

/* out = SignedSat(a + b) or SignedSat(a - b), with 1 in x2 (or ORed into it
 * when `accumulate`) if it saturated. The saturation direction is a's sign,
 * which is the result's true sign. Clobbers x1, x8 and the host flags;
 * `out` is not a source. */
void A32Compiler::saturating_add(unsigned out, unsigned a, unsigned b,
                                 bool subtract, bool accumulate) {
  emit(subtract ? enc::subs(out, a, b) : enc::adds(out, a, b));
  emit(enc::asr_i(temp1, a, 31));
  emit(enc::eor_i(temp1, temp1, field(0x7fff'ffffU)));
  emit(enc::csel(out, temp1, out, enc::vs));
  if (!accumulate) {
    emit(enc::cset(2, enc::vs));
  } else {
    emit(enc::cset(temp2, enc::vs));
    emit(enc::orr(2, 2, temp2));
  }
}

A32Compiler::Flow A32Compiler::emit_saturate(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  switch (info.op) {
  case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    clobber_flags();
    const bool subtract = info.op == Op::qsub || info.op == Op::qdsub;
    if (info.op == Op::qdadd || info.op == Op::qdsub) {
      saturating_add(temp3, n, n, false, false);
      saturating_add(temp0, m, temp3, subtract, true);
    } else {
      saturating_add(temp0, m, n, subtract, false);
    }
    or_q(2);
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::ssat:
  case Op::usat: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned sat = static_cast<unsigned>(o.saturate_to);
    const unsigned amount = static_cast<unsigned>(o.shift_n);
    if (o.shift_t == SRType::ASR)
      emit(enc::asr_i(temp0, n, amount >= 32U ? 31U : amount));
    else if (amount == 0U)
      emit(enc::mov(temp0, n));
    else
      emit(enc::lsl_i(temp0, n, amount));
    if (info.op == Op::ssat && sat == 32U) {
      // The whole range: nothing saturates.
      const auto dest = def(o.d, false, fallback_n);
      emit(enc::mov(dest.native.host, temp0));
      done(dest);
      return Flow::next;
    }
    clobber_flags();
    if (info.op == Op::ssat) {
      const std::uint32_t mask = (std::uint32_t{1} << (sat - 1U)) - 1U;
      emit(enc::sbfx(temp1, temp0, 0, sat));
      emit(enc::cmp(temp0, temp1));
      emit(enc::asr_i(2, temp0, 31));
      if (mask != 0U)
        emit(enc::eor_i(2, 2, field(mask)));
    } else {
      const std::uint32_t max = sat == 0U ? 0U : (std::uint32_t{1} << sat) - 1U;
      if (sat == 0U)
        emit(enc::movz(temp1, 0));
      else
        emit(enc::ubfx(temp1, temp0, 0, sat));
      emit(enc::cmp(temp0, temp1));
      emit(enc::asr_i(temp2, temp0, 31));
      load_u32(2, max);
      emit(enc::bic(2, 2, temp2));
    }
    emit(enc::csel(temp0, 2, temp0, enc::ne));
    emit(enc::cset(2, enc::ne));
    or_q(2);
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// The DSP multiplies.

/* out = the bottom (index 0) or top (index 1) halfword of source, sign
 * extended. */
void A32Compiler::emit_half(unsigned out, unsigned source, unsigned index) {
  emit(index == 0U ? enc::sxth(out, source) : enc::asr_i(out, source, 16));
}

/* x1 = (x0 != sign_extend(w0)): Q is set when a 64-bit result does not fit a
 * 32-bit one. Clobbers x1, x2 and the host flags. */
void A32Compiler::set_q_on_overflow() {
  clobber_flags();
  emit(enc::x::sxtw(temp1, temp0));
  emit(enc::x::cmp(temp0, temp1));
  emit(enc::cset(2, enc::ne));
  or_q(2);
}

A32Compiler::Flow A32Compiler::emit_dsp_multiply(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  switch (info.op) {
  case Op::smul_xy:
  case Op::smla_xy: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit_half(temp0, n, static_cast<unsigned>(o.n_index));
    emit_half(temp1, m, static_cast<unsigned>(o.m_index));
    emit(enc::mul(temp0, temp0, temp1));
    if (info.op == Op::smla_xy) {
      // The accumulation sets Q on signed overflow.
      const unsigned a = use(o.a, fallback_a);
      clobber_flags();
      emit(enc::adds(temp0, temp0, a));
      emit(enc::cset(2, enc::vs));
      or_q(2);
    }
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::smlal_xy: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit_half(temp0, n, static_cast<unsigned>(o.n_index));
    emit_half(temp1, m, static_cast<unsigned>(o.m_index));
    const unsigned lo = use(o.dLo, fallback_a);
    const unsigned hi = use(o.dHi, fallback_hi);
    emit(enc::x::orr(temp3, lo, hi, enc::lsl, 32));
    emit(enc::smaddl(temp0, temp0, temp1, temp3));
    const auto dlo = def(o.dLo, false, fallback_n);
    const auto dhi = def(o.dHi, false, fallback_m);
    emit(enc::mov(dlo.native.host, temp0));
    emit(enc::x::lsr_i(dhi.native.host, temp0, 32));
    done(dlo);
    done(dhi);
    return Flow::next;
  }
  case Op::smulw:
  case Op::smlaw: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit_half(temp1, m, o.m_high ? 1U : 0U);
    emit(enc::smull(temp0, n, temp1));
    if (info.op == Op::smlaw) {
      const unsigned a = use(o.a, fallback_a);
      emit(enc::x::sbfm(temp3, a, 48, 31)); // sign extend and shift left 16
      emit(enc::x::add(temp0, temp0, temp3));
    }
    emit(enc::x::asr_i(temp0, temp0, 16));
    if (info.op == Op::smlaw)
      set_q_on_overflow();
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::smlad: case Op::smlsd: case Op::smuad: case Op::smusd:
  case Op::smlald: case Op::smlsld: {
    const bool subtract = info.op == Op::smlsd || info.op == Op::smusd ||
                          info.op == Op::smlsld;
    const bool is_long = info.op == Op::smlald || info.op == Op::smlsld;
    const unsigned n = use(o.n, fallback_n);
    unsigned m = use(o.m, fallback_m);
    if (o.m_swap) {
      emit(enc::ror_i(temp1, m, 16));
      m = temp1;
    }
    // x0 = n.lo * m.lo, then plus or minus n.hi * m.hi.
    emit(enc::sxth(temp0, n));
    emit(enc::sxth(temp2, m));
    emit(enc::smull(temp0, temp0, temp2));
    emit(enc::asr_i(temp2, n, 16));
    emit(enc::asr_i(temp3, m, 16));
    emit(subtract ? enc::smsubl(temp0, temp2, temp3, temp0)
                  : enc::smaddl(temp0, temp2, temp3, temp0));
    if (is_long) {
      const unsigned lo = use(o.dLo, fallback_a);
      const unsigned hi = use(o.dHi, fallback_hi);
      emit(enc::x::orr(temp3, lo, hi, enc::lsl, 32));
      emit(enc::x::add(temp0, temp0, temp3));
      const auto dlo = def(o.dLo, false, fallback_n);
      const auto dhi = def(o.dHi, false, fallback_m);
      emit(enc::mov(dlo.native.host, temp0));
      emit(enc::x::lsr_i(dhi.native.host, temp0, 32));
      done(dlo);
      done(dhi);
      return Flow::next;
    }
    if (info.op == Op::smlad || info.op == Op::smlsd) {
      const unsigned a = use(o.a, fallback_a);
      emit(enc::x::add_sxtw(temp0, temp0, a));
    }
    if (info.op != Op::smusd)
      set_q_on_overflow();
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  case Op::smmul: case Op::smmla: case Op::smmls: {
    const unsigned n = use(o.n, fallback_n);
    const unsigned m = use(o.m, fallback_m);
    emit(enc::smull(temp0, n, m));
    if (info.op != Op::smmul) {
      const unsigned a = use(o.a, fallback_a);
      emit(enc::x::lsl_i(temp3, a, 32));
      emit(info.op == Op::smmla ? enc::x::add(temp0, temp0, temp3)
                                : enc::x::sub(temp0, temp3, temp0));
    }
    if (o.round) {
      emit(enc::x::movz(temp1, 0x8000, 1));
      emit(enc::x::add(temp0, temp0, temp1));
    }
    emit(enc::x::lsr_i(temp0, temp0, 32));
    const auto dest = def(o.d, false, fallback_n);
    emit(enc::mov(dest.native.host, temp0));
    done(dest);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Parallel add and subtract.
//
// The packed lanes of the two registers go through the vector unit as bytes
// or halfwords of a D register. No instruction here may set the host's QC
// flag, which is the guest's FPSCR.QC: the unsigned saturating forms use
// UMAX and UMIN (UQSUB is MAX(a, b) - b, UQADD is a + MIN(b, ~a)), and the
// signed forms take GE from the exact sum of a widening add. The signed
// saturating forms, which would need saturating clamps, stay in the library.
// ASX and SAX exchange the halves of Rm and take one lane from the
// subtraction and one from the addition.

A32Compiler::Flow A32Compiler::emit_parallel(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  if (info.mode == ParallelMode::saturating || o.d == 15 || o.n == 15 ||
      o.m == 15)
    return emit_arith_through_helper();
  constexpr unsigned v_a = 0;
  constexpr unsigned v_b = 1;
  constexpr unsigned v_add = 2;
  constexpr unsigned v_add_ge = 3;
  constexpr unsigned v_sub = 4;
  constexpr unsigned v_sub_ge = 5;
  constexpr unsigned v_wide = 6;
  const bool exchange =
      info.kind == ParallelKind::asx || info.kind == ParallelKind::sax;
  const bool halfwords = exchange || info.kind == ParallelKind::add16 ||
                         info.kind == ParallelKind::sub16;
  const unsigned size = halfwords ? 1U : 0U;
  const bool sets_ge = info.mode == ParallelMode::signed_wrap ||
                       info.mode == ParallelMode::unsigned_wrap;
  const unsigned n = use(o.n, fallback_n);
  const unsigned m = use(o.m, fallback_m);
  emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false, v_a, n));
  unsigned b_source = m;
  if (exchange) {
    emit(enc::ror_i(temp0, m, 16));
    b_source = temp0;
  }
  emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false, v_b, b_source));

  using enc::vec::Same;
  const auto three = [&](Same op, unsigned d, unsigned a, unsigned b) {
    emit(enc::vec::same(false, op, size, d, a, b));
  };
  const auto lanes = [&](bool subtract, unsigned result, unsigned ge) {
    switch (info.mode) {
    case ParallelMode::signed_wrap:
      three(subtract ? Same::sub : Same::add, result, v_a, v_b);
      emit(enc::vec::diff(false,
                          subtract ? enc::vec::ssubl : enc::vec::saddl, size,
                          v_wide, v_a, v_b));
      emit(enc::vec::misc(true, enc::vec::cmge0, size + 1U, v_wide, v_wide));
      emit(enc::vec::misc(false, enc::vec::xtn, size, ge, v_wide));
      break;
    case ParallelMode::unsigned_wrap:
      three(subtract ? Same::sub : Same::add, result, v_a, v_b);
      if (subtract)
        three(Same::cmhs, ge, v_a, v_b);
      else
        three(Same::cmhi, ge, v_a, result); // a > a + b: a carry
      break;
    case ParallelMode::unsigned_saturating:
      if (subtract) {
        three(Same::umax, result, v_a, v_b);
        three(Same::sub, result, result, v_b);
      } else {
        emit(enc::vec::misc(false, enc::vec::not_, 0, v_wide, v_a));
        three(Same::umin, v_wide, v_wide, v_b);
        three(Same::add, result, v_a, v_wide);
      }
      break;
    case ParallelMode::signed_halving:
      three(subtract ? Same::shsub : Same::shadd, result, v_a, v_b);
      break;
    case ParallelMode::unsigned_halving:
      three(subtract ? Same::uhsub : Same::uhadd, result, v_a, v_b);
      break;
    case ParallelMode::saturating:
      break;
    }
  };
  if (exchange) {
    lanes(false, v_add, v_add_ge);
    lanes(true, v_sub, v_sub_ge);
    // ASX subtracts in the low lane, SAX in the high one.
    const unsigned lane = info.kind == ParallelKind::asx ? 0U : 1U;
    emit(enc::vec::ins_element(1, v_add, lane, v_sub, lane));
    if (sets_ge)
      emit(enc::vec::ins_element(1, v_add_ge, lane, v_sub_ge, lane));
  } else {
    lanes(info.kind == ParallelKind::sub16 || info.kind == ParallelKind::sub8,
          v_add, v_add_ge);
  }
  if (sets_ge) {
    emit(enc::vec::float_int(enc::vec::fmov_to_w, 0, false, temp0, v_add_ge));
    emitter_.store_w(temp0, 15, offsetof(CpuState, a32_ge_mask));
  }
  const auto dest = def(o.d, false, fallback_n);
  emit(enc::vec::float_int(enc::vec::fmov_to_w, 0, false, dest.native.host,
                           v_add));
  done(dest);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// What the slow helper executes.

A32Compiler::Flow A32Compiler::emit_arith_through_helper() {
  emitter_.branch(cold_exit(ColdPath::Kind::slow));
  return Flow::end;
}

} // namespace uml::cpu::arm64::jit::a32
