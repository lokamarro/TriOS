// SPDX-License-Identifier: GPL-2.0-or-later
//
// Advanced SIMD: the dispatch, the operand plumbing shared by the families,
// and the operations on registers of one width, integer and floating point.
//
// A Q operand is its resident host register and a D operand is the low
// (even) or high (odd) half of one. Every D-register operation therefore runs
// as the 64-bit arrangement of the A64 instruction on sources moved to the
// scratch registers V0 and V1, writes its result to V5, and inserts it into
// the destination half, which keeps the other half. The operations A64 has no
// 64-bit arrangement for (the 64-bit lanes of the arithmetic, saturating and
// shift groups) run on the 128-bit arrangement with each D source in both
// halves, whose low half is the result.
//
// Floating-point operations run under the Standard FPSCR value, which the
// translation sets before the first one (set_fp_mode), and the saturating
// operations set QC in the host FPSR, which is why a block holding one owns
// the native floating-point environment.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

#include <array>
#include <bit>
#include <cstdlib>

namespace uml::cpu::arm64::jit::a32 {

// ---------------------------------------------------------------------------
// Operand plumbing.

unsigned A32Compiler::esize_log2(int esize) noexcept {
  return static_cast<unsigned>(
      std::countr_zero(static_cast<unsigned>(esize) / 8U));
}

void A32Compiler::vmov(unsigned d, unsigned n) {
  if (d != n)
    emit(enc::vec::bitwise_op(true, enc::vec::orr, d, n, n));
}

unsigned A32Compiler::vsrc(unsigned reg, bool quad, unsigned scratch) {
  return quad ? q_use(reg >> 1, scratch) : d_use(reg, scratch);
}

/* dest = op(n, m) on D or Q registers. `base` is the A64 instruction with its
 * register fields clear and its arrangement set for `quad || wide`; `wide`
 * runs a D operation on the 128-bit arrangement with both halves equal. An
 * operation that accumulates starts from the destination's value. */
void A32Compiler::neon_op3(std::uint32_t base, bool quad, bool wide,
                           unsigned d, unsigned n, unsigned m,
                           bool reads_dest) {
  if (quad) {
    const unsigned a = q_use(n >> 1, s_n);
    const unsigned b = q_use(m >> 1, s_m);
    const auto dest = q_def(d >> 1, reads_dest, s_result);
    emit_vec3(base, dest.host, a, b);
    q_done(dest, d >> 1);
    return;
  }
  const unsigned a = wide ? d_use_twice(n, s_n) : d_use(n, s_n);
  const unsigned b = wide ? d_use_twice(m, s_m) : d_use(m, s_m);
  if (reads_dest)
    vmov(s_result, wide ? d_use_twice(d, s_acc) : d_use(d, s_acc));
  emit_vec3(base, s_result, a, b);
  d_write(d, s_result);
}

void A32Compiler::neon_op2(std::uint32_t base, bool quad, bool wide,
                           unsigned d, unsigned m, bool reads_dest) {
  if (quad) {
    const unsigned a = q_use(m >> 1, s_m);
    const auto dest = q_def(d >> 1, reads_dest, s_result);
    emit_vec2(base, dest.host, a);
    q_done(dest, d >> 1);
    return;
  }
  const unsigned a = wide ? d_use_twice(m, s_m) : d_use(m, s_m);
  if (reads_dest)
    vmov(s_result, wide ? d_use_twice(d, s_acc) : d_use(d, s_acc));
  emit_vec2(base, s_result, a);
  d_write(d, s_result);
}

/* The Q register q takes `result` (a scratch register or a resident one). */
void A32Compiler::q_put(unsigned q, unsigned result) {
  const auto dest = q_def(q, false, result);
  vmov(dest.host, result);
  q_done(dest, q);
}

/* D or Q register `reg` takes `result`. */
void A32Compiler::neon_put(unsigned reg, bool quad, unsigned result) {
  if (quad)
    q_put(reg >> 1, result);
  else
    d_write(reg, result);
}

// ---------------------------------------------------------------------------
// The dispatch.

/* Whether the instruction is a half-precision variant of an Advanced SIMD
 * floating-point form. The variants share the form (and the mnemonic) of the
 * single-precision ones and differ in the operands' esize (16), so the
 * single-precision lowering cannot run them. */
bool A32Compiler::neon_half_precision(const VInfo &v) const {
  const auto &o = cur().inst.operands;
  if (o.esize != 16)
    return false;
  switch (v.mnemonic) {
  case VM::vfma: case VM::vfms: case VM::vacge: case VM::vacgt:
  case VM::vrecps: case VM::vrsqrts:
    return true;
  case VM::vceq:
    return v.variant == Variant::i ? o.floating_point : !o.int_operation;
  case VM::vcge:
    return v.variant == Variant::i ? o.floating_point
                                   : o.vtype_vcge == aarch32::VCGEType::fp;
  case VM::vcgt:
    return v.variant == Variant::i ? o.floating_point
                                   : o.vtype_vcgt == aarch32::VCGTtype::fp;
  case VM::vcle: case VM::vclt: case VM::vabs: case VM::vneg: case VM::vrecpe:
  case VM::vrsqrte:
    return o.floating_point;
  case VM::vcvt:
    return v.variant == Variant::is || v.variant == Variant::xs;
  default:
    return v.variant == Variant::f || o.floating_point;
  }
}

A32Compiler::Flow A32Compiler::emit_neon(const VInfo &v) {
  const auto &o = cur().inst.operands;
  if (neon_half_precision(v))
    return emit_arith_through_helper();
  switch (v.mnemonic) {
  case VM::vmul: case VM::vmla: case VM::vmls: case VM::vmlal:
  case VM::vmlsl: case VM::vmull: case VM::vqdmulh: case VM::vqrdmulh:
  case VM::vqdmull: case VM::vqdmlal: case VM::vqdmlsl:
    if (v.variant == Variant::s || o.scalar_form)
      return emit_neon_scalar(v);
    break;
  default:
    break;
  }
  switch (v.mnemonic) {
  case VM::vadd: case VM::vsub: case VM::vmul: case VM::vmla: case VM::vmls:
  case VM::vaba: case VM::vabd: case VM::vhadd: case VM::vhsub:
  case VM::vrhadd: case VM::vqadd: case VM::vqsub: case VM::vrshl:
  case VM::vqrshl: case VM::vmax: case VM::vmin: case VM::vpadd:
  case VM::vpmax: case VM::vpmin: case VM::vtst: case VM::vqdmulh:
  case VM::vqrdmulh: case VM::vfma: case VM::vfms: case VM::vacge:
  case VM::vacgt: case VM::vrecps: case VM::vrsqrts:
    return emit_neon_same(v);
  case VM::vshl: case VM::vqshl:
    return v.variant == Variant::i ? emit_neon_shift(v) : emit_neon_same(v);
  case VM::vceq: case VM::vcge: case VM::vcgt: case VM::vcle: case VM::vclt:
    return v.variant == Variant::i ? emit_neon_misc(v) : emit_neon_same(v);
  case VM::vand: case VM::vbic: case VM::vorr: case VM::vorn: case VM::veor:
  case VM::vbsl: case VM::vbit: case VM::vbif:
    return v.variant == Variant::i ? emit_neon_immediate(v)
                                   : emit_neon_logic(v);
  case VM::vmvn:
    return v.variant == Variant::i ? emit_neon_immediate(v)
                                   : emit_neon_logic(v);
  case VM::vmov:
    return emit_neon_immediate(v);
  case VM::vshr: case VM::vsra: case VM::vrshr: case VM::vrsra: case VM::vsli:
  case VM::vsri: case VM::vqshlu: case VM::vshrn: case VM::vrshrn:
  case VM::vqshrn: case VM::vqrshrn: case VM::vqshrun: case VM::vqrshrun:
  case VM::vshll: case VM::vmovl:
    return emit_neon_shift(v);
  case VM::vaddl: case VM::vaddw: case VM::vsubl: case VM::vsubw:
  case VM::vabal: case VM::vabdl: case VM::vmlal: case VM::vmlsl:
  case VM::vmull: case VM::vqdmull: case VM::vqdmlal: case VM::vqdmlsl:
  case VM::vaddhn: case VM::vsubhn: case VM::vraddhn: case VM::vrsubhn:
    return emit_neon_diff(v);
  case VM::vabs: case VM::vneg: case VM::vqabs: case VM::vqneg: case VM::vcls:
  case VM::vclz: case VM::vcnt: case VM::vrev16: case VM::vrev32:
  case VM::vrev64: case VM::vpaddl: case VM::vpadal: case VM::vrecpe:
  case VM::vrsqrte: case VM::vmovn: case VM::vqmovn: case VM::vqmovun:
  case VM::vcvt:
    return emit_neon_misc(v);
  case VM::vdup: case VM::vext: case VM::vswp: case VM::vtrn: case VM::vzip:
  case VM::vuzp: case VM::vtbl: case VM::vtbx:
    return emit_neon_permute(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Advanced SIMD floating point in the middle of VFP code.
//
// A Neon operation between VFP instructions would write the FPCR twice, and a
// write waits for the floating-point operations before it. The guest's FPCR
// and the Standard FPSCR value part only on a nonzero denormal operand, which
// the Standard value flushes and raises Input Denormal for, on a NaN operand,
// which it replaces by the default NaN, on a result that is tiny before
// rounding, which it flushes and raises Underflow for, and on a rounding mode
// other than to nearest. A run under the guest's FPCR leaves its exception
// flags behind, so what the Standard value would not raise must be found
// before it: the product test looks at the exponents. Integer operations
// test for those cases; the
// operation runs under the guest's FPCR when none is present, and otherwise
// under the Standard value from a cold path. VMAX and VMIN return an operand,
// so they need the first two tests only.

namespace {

/* Four of V0-V7 that are not in `live` (a bit set). The callers keep at most
 * four of them live. */
std::array<unsigned, 4> free_vector_scratch(unsigned live) {
  std::array<unsigned, 4> free{};
  unsigned count = 0;
  for (unsigned reg = 0; reg < 8U && count < free.size(); ++reg)
    if (((live >> reg) & 1U) == 0U)
      free[count++] = reg;
  if (count < free.size())
    std::abort(); // a caller keeps too many scratch registers live
  return free;
}

constexpr unsigned scratch_bit(unsigned reg) noexcept {
  return reg < 8U ? 1U << reg : 0U;
}

} // namespace

/* x9 is nonzero when a lane of `a` or `b` is a nonzero denormal or a NaN (the
 * low D of each for a D operation). Clobbers four of V0-V7 outside `live`
 * (and the operands), sets no flag and raises no exception. */
void A32Compiler::emit_float_special_test(unsigned a, unsigned b, bool quad,
                                          unsigned live) {
  const auto t = free_vector_scratch(live | scratch_bit(a) | scratch_bit(b));
  const unsigned absolute = t[0];
  const unsigned constant = t[1];
  const unsigned mask = t[2];
  const unsigned second = t[3];
  const auto mask_of = [&](unsigned x, unsigned out) {
    // out = lanes where |x| - 1 < 0x007fffff: a nonzero denormal.
    emit(enc::vec::float_misc(true, enc::vec::fabs_, false, absolute, x));
    emit(enc::vec::modified_immediate(true, 0, 0x0, 1, constant));
    emit(enc::vec::same(true, enc::vec::sub, 2, out, absolute, constant));
    emit(enc::vec::modified_immediate(true, 0, 0xd, 0x7f, constant));
    emit(enc::vec::same(true, enc::vec::cmhi, 2, out, constant, out));
    // constant = lanes where |x| > 0x7f800000: a NaN.
    emit(enc::movz(9, 0x7f80, 1));
    emit(enc::vec::dup_general(true, 2, constant, 9));
    emit(enc::vec::same(true, enc::vec::cmhi, 2, constant, absolute, constant));
    emit(enc::vec::bitwise_op(true, enc::vec::orr, out, out, constant));
  };
  if (quad) {
    mask_of(a, mask);
    mask_of(b, second);
    emit(enc::vec::bitwise_op(true, enc::vec::orr, mask, mask, second));
  } else {
    emit(enc::vec::permute(true, enc::vec::zip1, 3, mask, a, b));
    mask_of(mask, mask);
  }
  emit(enc::vec::same(true, enc::vec::umaxp, 2, mask, mask, mask));
  emit(enc::vec::umov(3, 9, mask, 0));
}

/* x9 is nonzero when a lane of the sum `result` is a nonzero denormal. A sum
 * of two normal numbers that small is exact, so the guest's FPCR raised no
 * flag for it, and the rerun under the Standard value, which flushes it and
 * raises Underflow, adds only what that run raises. */
void A32Compiler::emit_float_tiny_test(unsigned result, bool quad,
                                       unsigned live) {
  const auto t = free_vector_scratch(live | scratch_bit(result));
  const unsigned absolute = t[0];
  const unsigned constant = t[1];
  emit(enc::vec::float_misc(quad, enc::vec::fabs_, false, absolute, result));
  emit(enc::vec::modified_immediate(quad, 0, 0x0, 1, constant));
  emit(enc::vec::same(quad, enc::vec::sub, 2, absolute, absolute, constant));
  emit(enc::vec::modified_immediate(quad, 0, 0xd, 0x7f, constant));
  emit(enc::vec::same(quad, enc::vec::cmhi, 2, constant, constant, absolute));
  if (quad)
    emit(enc::vec::same(true, enc::vec::umaxp, 2, constant, constant, constant));
  emit(enc::vec::umov(3, 9, constant, 0));
}

/* x9 is nonzero when a lane of the product of `a` and `b` (neither a denormal
 * or a NaN) might be tiny: both operands are nonzero and their biased
 * exponents add up to less than 129, so the product is below the smallest
 * normal number or close enough to it to round into it. Such a product is
 * inexact and the guest's FPCR would raise Inexact for it where the Standard
 * value raises only Underflow, which a rerun cannot take back, so the test
 * comes before the multiplication. */
void A32Compiler::emit_float_product_test(unsigned a, unsigned b, bool quad,
                                          unsigned live) {
  const auto t = free_vector_scratch(live | scratch_bit(a) | scratch_bit(b));
  const unsigned x = t[0];
  const unsigned y = t[1];
  const unsigned z = t[2];
  emit(enc::vec::float_misc(quad, enc::vec::fabs_, false, x, a));
  emit(enc::vec::float_misc(quad, enc::vec::fabs_, false, y, b));
  emit(enc::vec::shift_imm(quad, enc::vec::ushr, 64U - 23U, x, x));
  emit(enc::vec::shift_imm(quad, enc::vec::ushr, 64U - 23U, y, y));
  emit(enc::vec::same(quad, enc::vec::umin, 2, z, x, y));
  emit(enc::vec::same(quad, enc::vec::cmtst, 2, z, z, z));
  emit(enc::vec::same(quad, enc::vec::add, 2, x, x, y));
  emit(enc::vec::modified_immediate(quad, 0, 0x0, 129, y));
  emit(enc::vec::same(quad, enc::vec::cmhi, 2, x, y, x));
  emit(enc::vec::bitwise_op(quad, enc::vec::and_, x, x, z));
  if (quad)
    emit(enc::vec::same(true, enc::vec::umaxp, 2, x, x, x));
  emit(enc::vec::umov(3, 9, x, 0));
}

/* dest = a op b (`base` is the A64 instruction with its register fields
 * clear), run under the host FPCR as it stands, which is the guest's, when
 * the guest rounds to nearest and neither operand nor result is one of the
 * values above. dest is not a or b. Clobbers x0, x1 and x9. */
void A32Compiler::emit_guarded_float_op(std::uint32_t base, bool quad,
                                        unsigned dest, unsigned a, unsigned b,
                                        bool product, unsigned live) {
  const auto slow = make_label();
  const auto done = make_label();
  emitter_.load_w(0, 15, offsetof(CpuState, fpcr));
  emit(enc::ubfx(0, 0, 22, 2)); // FPCR.RMode
  emitter_.compare_zero(slow, 0, false, true);
  emit_float_special_test(a, b, quad, live | scratch_bit(dest));
  emitter_.compare_zero(slow, 9, true, true);
  if (product) {
    emit_float_product_test(a, b, quad, live | scratch_bit(dest));
    emitter_.compare_zero(slow, 9, true, true);
  }
  emit_vec3(base, dest, a, b);
  if (!product) {
    emit_float_tiny_test(dest, quad, live | scratch_bit(a) | scratch_bit(b));
    emitter_.compare_zero(slow, 9, true, true);
  }
  bind(done);
  float_cold_.push_back({.label = slow,
                         .continuation = done,
                         .base = base,
                         .quad = quad,
                         .dest = dest,
                         .a = a,
                         .b = b});
}

/* Whether the Neon operation is one of at most eight Advanced SIMD floating
 * point operations that a VFP instruction follows (or the end of the block,
 * after which the loop may go on with one): then guarding each is better than
 * bracketing the run by two FPCR writes (about the cost of three guards, a
 * write waiting for every floating-point operation before it). A longer run
 * is better off with one switch. Where the mode is not known, a guarded
 * operation tests the byte first, so in a loop of Neon instructions, where the
 * host FPCR stays Standard from one lap to the next, it costs a load and a
 * branch more than the switch it replaces. */
bool A32Compiler::neon_float_isolated() const {
  unsigned run = 1;
  for (std::size_t index = count_ + 1U; index < block_.size(); ++index) {
    const auto &inst = block_[index].inst;
    if (inst.status != aarch32::Status::ok ||
        op_info(inst.form).op != Op::vector)
      continue;
    const auto &next = vector_info(inst.form);
    switch (next.mnemonic) {
    case VM::vldr: case VM::vstr: case VM::vldm: case VM::vldmdb:
    case VM::vstm: case VM::vstmdb: case VM::fldmiax: case VM::fldmdbx:
    case VM::fstmiax: case VM::fstmdbx: case VM::vmov: case VM::vmrs:
    case VM::vmsr:
      continue;
    default:
      break;
    }
    if (next.vfp)
      return true;
    if (next.variant == Variant::f || next.mnemonic == VM::vfma ||
        next.mnemonic == VM::vfms || next.mnemonic == VM::vacge ||
        next.mnemonic == VM::vacgt || next.mnemonic == VM::vrecps ||
        next.mnemonic == VM::vrsqrts || next.mnemonic == VM::vrecpe ||
        next.mnemonic == VM::vrsqrte || next.mnemonic == VM::vcvt) {
      if (++run > 8U)
        return false;
    }
  }
  return true;
}

A32Compiler::Flow A32Compiler::emit_neon_float_guarded(const VInfo &v) {
  ++guarded_float_ops_;
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  using enc::vec::FloatSame;
  const bool accumulating = v.mnemonic == VM::vmla || v.mnemonic == VM::vmls;
  const FloatSame first = v.mnemonic == VM::vadd   ? enc::vec::fadd
                          : v.mnemonic == VM::vsub ? enc::vec::fsub
                                                   : enc::vec::fmul;
  const FloatSame second =
      v.mnemonic == VM::vmla ? enc::vec::fadd : enc::vec::fsub;
  const auto base = [&](FloatSame op) {
    return enc::vec::float_same(quad, op, false, 0, 0, 0);
  };
  const bool product = first == enc::vec::fmul;
  const unsigned a = vsrc(n, quad, s_n);
  const unsigned b = vsrc(m, quad, s_m);
  const unsigned old = accumulating ? vsrc(d, quad, s_acc) : 0U;
  const unsigned live = scratch_bit(a) | scratch_bit(b) | scratch_bit(old);

  /* Where the mode is not known, a Standard host runs the operations as they
   * are. */
  const bool tested = fp_mode_ == FpMode::unknown;
  Arm64Emitter::Label standard{};
  const auto done = make_label();
  if (tested) {
    standard = make_label();
    emitter_.load_b(0, 15, offsetof(CpuState, a32_fp_standard));
    emitter_.compare_zero(standard, 0, false, true);
    fp_mode_ = FpMode::guest;
  }
  if (accumulating) {
    emit_guarded_float_op(base(enc::vec::fmul), quad, s_extra, a, b, true,
                          live);
    emit_guarded_float_op(base(second), quad, s_result, old, s_extra, false,
                          scratch_bit(old));
  } else {
    emit_guarded_float_op(base(first), quad, s_result, a, b, product, live);
  }
  if (tested) {
    emitter_.branch(done);
    bind(standard);
    if (accumulating) {
      emit_vec3(base(enc::vec::fmul), s_extra, a, b);
      emit_vec3(base(second), s_result, old, s_extra);
    } else {
      emit_vec3(base(first), s_result, a, b);
    }
    fp_mode_ = FpMode::unknown;
  }
  bind(done);
  neon_put(d, quad, s_result);
  return Flow::next;
}

A32Compiler::Flow A32Compiler::emit_neon_minmax_guarded(const VInfo &v) {
  ++guarded_float_ops_;
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  const auto base = enc::vec::float_same(
      quad, v.mnemonic == VM::vmax ? enc::vec::fmax : enc::vec::fmin, false,
      0, 0, 0);
  const unsigned a = vsrc(n, quad, s_n);
  const unsigned b = vsrc(m, quad, s_m);
  const auto dest = quad ? q_def(d >> 1, false, s_result)
                         : NativeVector{s_result, false};
  const auto done = make_label();
  /* Where the mode is not known, the Standard value (set already) runs the
   * operation as it is. */
  const bool tested = fp_mode_ == FpMode::unknown;
  Arm64Emitter::Label standard{};
  if (tested) {
    standard = make_label();
    emitter_.load_b(0, 15, offsetof(CpuState, a32_fp_standard));
    emitter_.compare_zero(standard, 0, false, true);
    fp_mode_ = FpMode::guest;
  }
  emit_float_special_test(a, b, quad, scratch_bit(dest.host));
  const auto fast = make_label();
  emitter_.compare_zero(fast, 9, true, false);
  set_fp_mode(true);
  emit_vec3(base, dest.host, a, b);
  /* The guard's own return to the guest's value, not a need for it. */
  const auto guest_ops = guest_fp_mode_ops_;
  set_fp_mode(false);
  guest_fp_mode_ops_ = guest_ops;
  emitter_.branch(done);
  bind(fast);
  emit_vec3(base, dest.host, a, b);
  if (tested) {
    emitter_.branch(done);
    bind(standard);
    emit_vec3(base, dest.host, a, b);
    fp_mode_ = FpMode::unknown;
  }
  bind(done);
  if (quad)
    q_done(dest, d >> 1);
  else
    d_write(d, s_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Three registers of the same width.

A32Compiler::Flow A32Compiler::emit_neon_same(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  using enc::vec::FloatSame;
  using enc::vec::Same;

  const bool is_float =
      v.variant == Variant::f || v.mnemonic == VM::vfma ||
      v.mnemonic == VM::vfms || v.mnemonic == VM::vacge ||
      v.mnemonic == VM::vacgt || v.mnemonic == VM::vrecps ||
      v.mnemonic == VM::vrsqrts ||
      (v.mnemonic == VM::vceq && !o.int_operation) ||
      (v.mnemonic == VM::vcge && o.vtype_vcge == aarch32::VCGEType::fp) ||
      (v.mnemonic == VM::vcgt && o.vtype_vcgt == aarch32::VCGTtype::fp);
  if (is_float) {
    touch_fp_environment();
    const bool guardable = fp_mode_ != FpMode::standard &&
                           (v.mnemonic == VM::vmax || v.mnemonic == VM::vmin ||
                            v.mnemonic == VM::vadd || v.mnemonic == VM::vsub ||
                            v.mnemonic == VM::vmul || v.mnemonic == VM::vmla ||
                            v.mnemonic == VM::vmls) &&
                           neon_float_isolated();
    if (guardable && (v.mnemonic == VM::vmax || v.mnemonic == VM::vmin))
      return emit_neon_minmax_guarded(v);
    if (guardable)
      return emit_neon_float_guarded(v);
    set_fp_mode(true);
    FloatSame op = enc::vec::fadd;
    bool accumulate = false;
    switch (v.mnemonic) {
    case VM::vadd: op = enc::vec::fadd; break;
    case VM::vsub: op = enc::vec::fsub; break;
    case VM::vmul: op = enc::vec::fmul; break;
    case VM::vabd: op = enc::vec::fabd; break;
    case VM::vmax: op = enc::vec::fmax; break;
    case VM::vmin: op = enc::vec::fmin; break;
    case VM::vpadd: op = enc::vec::faddp; break;
    case VM::vpmax: op = enc::vec::fmaxp; break;
    case VM::vpmin: op = enc::vec::fminp; break;
    case VM::vrecps:
    case VM::vrsqrts:
      return emit_neon_step(v.mnemonic == VM::vrsqrts, quad, d, n, m);
    case VM::vacge: op = enc::vec::facge; break;
    case VM::vacgt: op = enc::vec::facgt; break;
    case VM::vceq: op = enc::vec::fcmeq; break;
    case VM::vcge: op = enc::vec::fcmge; break;
    case VM::vcgt: op = enc::vec::fcmgt; break;
    case VM::vfma: op = enc::vec::fmla; accumulate = true; break;
    case VM::vfms: op = enc::vec::fmls; accumulate = true; break;
    case VM::vmla:
    case VM::vmls: {
      // The product is rounded before it is added: two operations.
      const unsigned a = vsrc(n, quad, s_n);
      const unsigned b = vsrc(m, quad, s_m);
      emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
                s_extra, a, b);
      const unsigned old = vsrc(d, quad, s_acc);
      emit_vec3(enc::vec::float_same(quad,
                                     v.mnemonic == VM::vmla ? enc::vec::fadd
                                                            : enc::vec::fsub,
                                     false, 0, 0, 0),
                s_result, old, s_extra);
      neon_put(d, quad, s_result);
      return Flow::next;
    }
    default:
      exit_illegal();
      return Flow::end;
    }
    neon_op3(enc::vec::float_same(quad, op, false, 0, 0, 0), quad, false, d, n,
             m, accumulate);
    return Flow::next;
  }

  const unsigned size = esize_log2(o.esize);
  const bool wide64 = !quad && o.esize == 64;
  Same op = enc::vec::add;
  bool swap = false;
  bool accumulate = false;
  bool saturating = false;
  const bool u = o.unsigned_;
  switch (v.mnemonic) {
  case VM::vadd: op = enc::vec::add; break;
  case VM::vsub: op = enc::vec::sub; break;
  case VM::vmul: op = o.polynomial ? enc::vec::pmul : enc::vec::mul; break;
  case VM::vmla: op = enc::vec::mla; accumulate = true; break;
  case VM::vmls: op = enc::vec::mls; accumulate = true; break;
  case VM::vaba: op = u ? enc::vec::uaba : enc::vec::saba; accumulate = true; break;
  case VM::vabd: op = u ? enc::vec::uabd : enc::vec::sabd; break;
  case VM::vhadd: op = u ? enc::vec::uhadd : enc::vec::shadd; break;
  case VM::vhsub: op = u ? enc::vec::uhsub : enc::vec::shsub; break;
  case VM::vrhadd: op = u ? enc::vec::urhadd : enc::vec::srhadd; break;
  case VM::vqadd: op = u ? enc::vec::uqadd : enc::vec::sqadd; saturating = true; break;
  case VM::vqsub: op = u ? enc::vec::uqsub : enc::vec::sqsub; saturating = true; break;
  case VM::vshl: op = u ? enc::vec::ushl : enc::vec::sshl; swap = true; break;
  case VM::vrshl: op = u ? enc::vec::urshl : enc::vec::srshl; swap = true; break;
  case VM::vqshl: op = u ? enc::vec::uqshl : enc::vec::sqshl; swap = true; saturating = true; break;
  case VM::vqrshl: op = u ? enc::vec::uqrshl : enc::vec::sqrshl; swap = true; saturating = true; break;
  case VM::vmax: op = u ? enc::vec::umax : enc::vec::smax; break;
  case VM::vmin: op = u ? enc::vec::umin : enc::vec::smin; break;
  case VM::vpadd: op = enc::vec::addp; break;
  case VM::vpmax: op = u ? enc::vec::umaxp : enc::vec::smaxp; break;
  case VM::vpmin: op = u ? enc::vec::uminp : enc::vec::sminp; break;
  case VM::vtst: op = enc::vec::cmtst; break;
  case VM::vqdmulh: op = enc::vec::sqdmulh; saturating = true; break;
  case VM::vqrdmulh: op = enc::vec::sqrdmulh; saturating = true; break;
  case VM::vceq: op = enc::vec::cmeq; break;
  case VM::vcge:
    op = o.vtype_vcge == aarch32::VCGEType::unsigned_ ? enc::vec::cmhs
                                                      : enc::vec::cmge;
    break;
  case VM::vcgt:
    op = o.vtype_vcgt == aarch32::VCGTtype::unsigned_ ? enc::vec::cmhi
                                                      : enc::vec::cmgt;
    break;
  default:
    exit_illegal();
    return Flow::end;
  }
  if (saturating)
    touch_fp_environment();
  neon_op3(enc::vec::same(quad || wide64, op, size, 0, 0, 0), quad, wide64, d,
           swap ? m : n, swap ? n : m, accumulate);
  return Flow::next;
}

/* VRECPS and VRSQRTS. A64 FRECPS and FRSQRTS fuse the multiplication, which
 * rounds and raises flags differently, so the step is the product rounded
 * first and then 2 - p (or (3 - p) / 2). An infinity times a zero (a
 * denormal is a zero under the Standard FPSCR) gives 2.0 (or 1.5) without
 * raising Invalid Operation, which the multiplication would. The infinite
 * operand of such a lane is replaced by 1.0 before the product, so the
 * other operand still raises Input Denormal, and the constant takes the
 * lane afterwards. The classification is on integer images of the operands
 * so that it raises nothing. */
A32Compiler::Flow A32Compiler::emit_neon_step(bool rsqrt, bool quad, unsigned d,
                                              unsigned n, unsigned m) {
  using enc::vec::Logical;
  const unsigned a = vsrc(n, quad, s_n);
  const unsigned b = vsrc(m, quad, s_m);
  constexpr unsigned a_sign = 2, b_sign = 3, t_inf = 4, t_mask = 6, t_special = 7;
  const auto fmov = [&](unsigned reg, unsigned imm8) {
    emit(enc::vec::modified_immediate(quad, 0, 15, imm8, reg));
  };
  emit_vec2(enc::vec::shift_imm(quad, enc::vec::shl, 33, 0, 0), a_sign, a);
  emit_vec2(enc::vec::shift_imm(quad, enc::vec::shl, 33, 0, 0), b_sign, b);
  // |x| << 1 of an infinity is 0xff000000, and below 0x01000000 for a zero.
  emit(enc::vec::modified_immediate(quad, 0, 6, 0xff, t_inf));
  emit_vec3(enc::vec::same(quad, enc::vec::cmeq, 2, 0, 0, 0), t_mask, a_sign,
            t_inf);
  emit_vec3(enc::vec::same(quad, enc::vec::cmeq, 2, 0, 0, 0), t_inf, b_sign,
            t_inf);
  emit(enc::vec::modified_immediate(quad, 0, 6, 0x01, t_special));
  emit_vec3(enc::vec::same(quad, enc::vec::cmhi, 2, 0, 0, 0), a_sign, t_special,
            a_sign);
  emit_vec3(enc::vec::same(quad, enc::vec::cmhi, 2, 0, 0, 0), b_sign, t_special,
            b_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::and_, 0, 0, 0), t_mask, t_mask,
            b_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::and_, 0, 0, 0), t_inf, t_inf,
            a_sign);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::orr, 0, 0, 0), t_special,
            t_mask, t_inf);
  fmov(a_sign, 0x70); // 1.0
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bsl, 0, 0, 0), t_mask, a_sign,
            a);
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bsl, 0, 0, 0), t_inf, a_sign,
            b);
  emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
            s_result, t_mask, t_inf);
  fmov(a_sign, rsqrt ? 0x08 : 0x00); // 3.0 or 2.0
  emit_vec3(enc::vec::float_same(quad, enc::vec::fsub, false, 0, 0, 0),
            s_result, a_sign, s_result);
  if (rsqrt) {
    fmov(a_sign, 0x60); // 0.5
    emit_vec3(enc::vec::float_same(quad, enc::vec::fmul, false, 0, 0, 0),
              s_result, s_result, a_sign);
    fmov(a_sign, 0x78); // 1.5
  } else {
    fmov(a_sign, 0x00); // 2.0
  }
  emit_vec3(enc::vec::bitwise_op(quad, Logical::bit, 0, 0, 0), s_result, a_sign,
            t_special);
  neon_put(d, quad, s_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// Bitwise operations on whole registers.

A32Compiler::Flow A32Compiler::emit_neon_logic(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool quad = v.quad;
  using enc::vec::Logical;
  if (v.mnemonic == VM::vmvn) {
    neon_op2(enc::vec::misc(quad, enc::vec::not_, 0, 0, 0), quad, false, d, m,
             false);
    return Flow::next;
  }
  Logical op = enc::vec::and_;
  bool accumulate = false;
  switch (v.mnemonic) {
  case VM::vand: op = enc::vec::and_; break;
  case VM::vbic: op = enc::vec::bic; break;
  case VM::vorr: op = enc::vec::orr; break;
  case VM::vorn: op = enc::vec::orn; break;
  case VM::veor: op = enc::vec::eor; break;
  case VM::vbsl: op = enc::vec::bsl; accumulate = true; break;
  case VM::vbit: op = enc::vec::bit; accumulate = true; break;
  case VM::vbif: op = enc::vec::bif; accumulate = true; break;
  default:
    exit_illegal();
    return Flow::end;
  }
  neon_op3(enc::vec::bitwise_op(quad, op, 0, 0, 0), quad, false, d, n, m,
           accumulate);
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
