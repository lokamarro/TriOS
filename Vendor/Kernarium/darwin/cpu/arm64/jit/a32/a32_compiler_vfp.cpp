// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFP: the scalar floating-point arithmetic, comparison and conversion
// instructions on S and D registers under the guest's FPCR, the transfers
// between core and floating-point registers, and FPSCR.
//
// The operands are read into scratch registers (V3, V4 and V6 for the
// sources, V5 for the result), the A64 scalar instruction runs on them, and
// the result is inserted into its lane. FPSCR is split as the CpuState holds
// it: the control bits in fpcr, the cumulative flags and QC in the host FPSR
// while the native environment is active, and N, Z, C and V in
// a32_fpscr_nzcv, which a VCMP writes and VMRS APSR_nzcv, FPSCR reads.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"
#include "cpu/arm64/aarch32/state_format.hpp"

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr unsigned scratch_d = 6;

} // namespace

unsigned A32Compiler::fp_result(unsigned r, bool dp) noexcept {
  return dp || !s_shadowing_ ? scratch_result : take_s_shadow(r);
}

A32Compiler::Flow A32Compiler::emit_vfp(const OpInfo &, const VInfo &v) {
  switch (v.mnemonic) {
  case VM::vabs: case VM::vneg: case VM::vsqrt: case VM::vadd: case VM::vsub:
  case VM::vmul: case VM::vdiv: case VM::vnmul: case VM::vmla: case VM::vmls:
  case VM::vnmla: case VM::vnmls: case VM::vfma: case VM::vfms:
  case VM::vfnma: case VM::vfnms:
    return emit_vfp_arith(v);
  case VM::vcmp: case VM::vcmpe:
    return emit_vfp_compare(v);
  case VM::vcvt: case VM::vcvtb: case VM::vcvtt: case VM::vcvtr:
    return emit_vfp_convert(v);
  case VM::vmov:
    return emit_vfp_move(v);
  case VM::vmrs: case VM::vmsr:
    return emit_fpscr(v);
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Arithmetic.

A32Compiler::Flow A32Compiler::emit_vfp_arith(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const bool dp = v.dp;
  const unsigned type = dp ? 1U : 0U;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_fp_environment();
  set_fp_mode(false);
  switch (v.mnemonic) {
  case VM::vabs:
  case VM::vneg:
  case VM::vsqrt: {
    const unsigned source = fp_use(m, dp, scratch_m);
    const unsigned result = fp_result(d, dp);
    emit(enc::vec::float1(v.mnemonic == VM::vabs   ? enc::vec::fabs_s
                          : v.mnemonic == VM::vneg ? enc::vec::fneg_s
                                                   : enc::vec::fsqrt_s,
                          type, result, source));
    fp_write(d, dp, result);
    return Flow::next;
  }
  case VM::vadd: case VM::vsub: case VM::vmul: case VM::vdiv: case VM::vnmul: {
    const unsigned left = fp_use(n, dp, scratch_n);
    const unsigned right = fp_use(m, dp, scratch_m);
    enc::vec::Float2 op = enc::vec::fadd_s;
    switch (v.mnemonic) {
    case VM::vsub: op = enc::vec::fsub_s; break;
    case VM::vmul: op = enc::vec::fmul_s; break;
    case VM::vdiv: op = enc::vec::fdiv_s; break;
    case VM::vnmul: op = enc::vec::fnmul_s; break;
    default: break;
    }
    const unsigned result = fp_result(d, dp);
    emit(enc::vec::float2(op, type, result, left, right));
    fp_write(d, dp, result);
    return Flow::next;
  }
  case VM::vmla: case VM::vmls: case VM::vnmla: case VM::vnmls: {
    // The product is rounded before it is added: sum = (+-d) + (+-product).
    const unsigned left = fp_use(n, dp, scratch_n);
    const unsigned right = fp_use(m, dp, scratch_m);
    const unsigned addend = fp_use(d, dp, scratch_d);
    emit(enc::vec::float2(enc::vec::fmul_s, type, scratch_result, left, right));
    const bool negate_product =
        v.mnemonic == VM::vmls || v.mnemonic == VM::vnmla;
    const bool negate_addend =
        v.mnemonic == VM::vnmla || v.mnemonic == VM::vnmls;
    if (negate_product)
      emit(enc::vec::float1(enc::vec::fneg_s, type, scratch_result,
                            scratch_result));
    unsigned first = addend;
    if (negate_addend) {
      emit(enc::vec::float1(enc::vec::fneg_s, type, scratch_d, addend));
      first = scratch_d;
    }
    const unsigned result = fp_result(d, dp);
    emit(enc::vec::float2(enc::vec::fadd_s, type, result, first,
                          scratch_result));
    fp_write(d, dp, result);
    return Flow::next;
  }
  case VM::vfma: case VM::vfms: case VM::vfnma: case VM::vfnms: {
    const unsigned left = fp_use(n, dp, scratch_n);
    const unsigned right = fp_use(m, dp, scratch_m);
    const unsigned addend = fp_use(d, dp, scratch_d);
    // A64: FMADD d + n*m, FMSUB d - n*m, FNMADD -d - n*m, FNMSUB -d + n*m.
    const auto op = v.mnemonic == VM::vfma    ? enc::vec::fmadd
                    : v.mnemonic == VM::vfms  ? enc::vec::fmsub
                    : v.mnemonic == VM::vfnma ? enc::vec::fnmadd
                                              : enc::vec::fnmsub;
    const unsigned result = fp_result(d, dp);
    emit(enc::vec::float3(op, type, result, left, right, addend));
    fp_write(d, dp, result);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// VCMP and VCMPE. The A64 comparison sets the host NZCV, which are the bits
// FPSCR keeps in a32_fpscr_nzcv, and the guest's own flags are in CpuState.

namespace {
/* The instruction runs whatever the flags are. */
bool is_unconditional(const aarch32::Instruction &inst) noexcept {
  return inst.cond >= 14U;
}
} // namespace

A32Compiler::Flow A32Compiler::emit_vfp_compare(const VInfo &v) {
  const auto &o = cur().inst.operands;
  const bool dp = v.dp;
  touch_fp_environment();
  set_fp_mode(false);
  const unsigned left = fp_use(static_cast<unsigned>(o.d), dp, scratch_n);
  unsigned right = 0;
  if (!o.with_zero)
    right = fp_use(static_cast<unsigned>(o.m), dp, scratch_m);
  clobber_flags();
  emit(enc::vec::fcmp(dp ? 1U : 0U, left, right, o.quiet_nan_exc,
                      o.with_zero));
  emit(0xd53b'4200U); // mrs x0, nzcv
  emitter_.store_w(0, 15, offsetof(CpuState, a32_fpscr_nzcv));
  /* The VMRS that moves them to the APSR next finds them in the host NZCV,
   * where a reload through memory would only delay the branch on them. */
  if (is_unconditional(cur().inst) && is_fpscr_flags_read(count_ + 1U))
    flags_held_by_vmrs_ = count_ + 1U;
  return Flow::next;
}

bool A32Compiler::is_fpscr_flags_read(std::size_t index) const {
  if (index >= block_.size())
    return false;
  const auto &inst = block_[index].inst;
  if (inst.status != aarch32::Status::ok || !is_unconditional(inst) ||
      op_info(inst.form).op != Op::vector)
    return false;
  const auto &v = vector_info(inst.form);
  return v.mnemonic == VM::vmrs && inst.operands.t == 15;
}

// ---------------------------------------------------------------------------
// Conversions.

A32Compiler::Flow A32Compiler::emit_vfp_convert(const VInfo &v) {
  const auto &o = cur().inst.operands;
  touch_fp_environment();
  const auto fmov_to_w = [&](unsigned source) {
    emit(enc::vec::float_int(enc::vec::fmov_to_w, 0, false, 0, source));
  };
  const auto fmov_from_w = [&](unsigned destination) {
    emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false, destination, 0));
  };
  if (v.mnemonic == VM::vcvtb || v.mnemonic == VM::vcvtt) {
    // Between single and half precision, in the bottom or top half of S.
    set_fp_mode(false);
    const unsigned half_lane = static_cast<unsigned>(o.lowbit) / 16U;
    if (o.convert_from_half) {
      const unsigned m = static_cast<unsigned>(o.m);
      const unsigned host = read_native_vector(m >> 2, scratch_m).host;
      emit(enc::vec::dup_scalar(1, scratch_n, host, 2U * (m & 3U) + half_lane));
      emit(enc::vec::fcvt(0, 3, scratch_result, scratch_n));
      s_write(static_cast<unsigned>(o.d), scratch_result);
    } else {
      const unsigned source = s_use(static_cast<unsigned>(o.m), scratch_n);
      emit(enc::vec::fcvt(3, 0, scratch_result, source));
      const unsigned d = static_cast<unsigned>(o.d);
      const auto destination = q_def(d >> 2, true, 2);
      emit(enc::vec::ins_element(1, destination.host,
                                 2U * (d & 3U) + half_lane, scratch_result, 0));
      q_done(destination, d >> 2);
    }
    return Flow::next;
  }
  switch (v.variant) {
  case Variant::ds:
  case Variant::sd: {
    // Between double and single precision.
    set_fp_mode(false);
    const bool to_single = o.double_to_single;
    const unsigned source = fp_use(static_cast<unsigned>(o.m), to_single,
                                   scratch_m);
    emit(enc::vec::fcvt(to_single ? 0U : 1U, to_single ? 1U : 0U,
                        scratch_result, source));
    fp_write(static_cast<unsigned>(o.d), !to_single, scratch_result);
    return Flow::next;
  }
  case Variant::uiv:
  case Variant::siv:
  case Variant::vi: {
    // Between floating point and a 32-bit integer in an S register.
    set_fp_mode(false);
    const bool dp = o.esize == 64;
    const unsigned type = dp ? 1U : 0U;
    if (o.to_integer) {
      const unsigned source = fp_use(static_cast<unsigned>(o.m), dp, scratch_m);
      using enc::vec::FloatInt;
      using enc::vec::FloatMisc;
      const bool is_unsigned = o.unsigned_;
      const unsigned result = fp_result(static_cast<unsigned>(o.d), false);
      /* A single converts in the vector unit, S to S: the A64 scalar form
       * rounds, saturates and raises the flags as the general-register form
       * does, without the two transfers through W0. A double's 32-bit result
       * goes through W0. */
      const auto convert = [&](FloatInt signed_op, FloatInt unsigned_op,
                               FloatMisc signed_scalar,
                               FloatMisc unsigned_scalar) {
        if (dp)
          emit(enc::vec::float_int(is_unsigned ? unsigned_op : signed_op,
                                   type, false, 0, source));
        else
          emit(enc::vec::float_misc_scalar(
              is_unsigned ? unsigned_scalar : signed_scalar, false, result,
              source));
      };
      if (o.zero_rounding) {
        convert(enc::vec::fcvtzs_w, enc::vec::fcvtzu_w, enc::vec::fcvtzs,
                enc::vec::fcvtzu);
      } else {
        // VCVTR rounds as FPSCR says: one conversion of the matching mode,
        // which also raises exactly the flags the architecture does.
        clobber_flags();
        const auto nearest = make_label();
        const auto plus = make_label();
        const auto minus = make_label();
        const auto done = make_label();
        emitter_.load_w(2, 15, offsetof(CpuState, fpcr));
        emit(enc::ubfx(2, 2, 22, 2));
        compare_zero(nearest, 2, false, false);
        emit(enc::cmp_i(2, 1));
        branch_condition(plus, enc::eq);
        emit(enc::cmp_i(2, 2));
        branch_condition(minus, enc::eq);
        convert(enc::vec::fcvtzs_w, enc::vec::fcvtzu_w, enc::vec::fcvtzs,
                enc::vec::fcvtzu);
        emitter_.branch(done);
        bind(nearest);
        convert(enc::vec::fcvtns_w, enc::vec::fcvtnu_w, enc::vec::fcvtns,
                enc::vec::fcvtnu);
        emitter_.branch(done);
        bind(plus);
        convert(enc::vec::fcvtps_w, enc::vec::fcvtpu_w, enc::vec::fcvtps,
                enc::vec::fcvtpu);
        emitter_.branch(done);
        bind(minus);
        convert(enc::vec::fcvtms_w, enc::vec::fcvtmu_w, enc::vec::fcvtms,
                enc::vec::fcvtmu);
        bind(done);
      }
      if (dp)
        fmov_from_w(result);
      s_write(static_cast<unsigned>(o.d), result);
    } else {
      const unsigned source = s_use(static_cast<unsigned>(o.m), scratch_m);
      const unsigned result = fp_result(static_cast<unsigned>(o.d), dp);
      if (dp) {
        fmov_to_w(source);
        emit(enc::vec::float_int(o.unsigned_ ? enc::vec::ucvtf_w
                                             : enc::vec::scvtf_w,
                                 type, false, result, 0));
      } else {
        emit(enc::vec::float_misc_scalar(
            o.unsigned_ ? enc::vec::ucvtf : enc::vec::scvtf, false, result,
            source));
      }
      fp_write(static_cast<unsigned>(o.d), dp, result);
    }
    return Flow::next;
  }
  case Variant::toxv:
  case Variant::xv: {
    // Between floating point and fixed point, in place.
    const bool dp = o.fp_size == 64;
    const unsigned type = dp ? 1U : 0U;
    const unsigned d = static_cast<unsigned>(o.d);
    const unsigned fraction = static_cast<unsigned>(o.frac_bits);
    const unsigned source = fp_use(d, dp, scratch_m);
    if (o.to_fixed) {
      set_fp_mode(false);
      const auto convert = o.unsigned_ ? enc::vec::fcvtzu_w : enc::vec::fcvtzs_w;
      const bool narrow = o.size == 16;
      if (narrow) {
        clobber_flags();
        emit(0xd53b'4421U); // mrs x1, fpsr
      }
      emit(fraction == 0U ? enc::vec::float_int(convert, type, false, 0, source)
                          : enc::vec::float_fixed(convert, type, fraction, 0,
                                                  source));
      if (narrow) {
        // The 16-bit result saturates, and a saturation raises only IOC.
        emit(0xd53b'4422U); // mrs x2, fpsr
        /* Only scratch registers: the host registers x3-x28 may hold the
         * guest's. */
        constexpr unsigned limit = 9, result = 10, status = 11;
        if (o.unsigned_) {
          emit(enc::movz(limit, 0xffffU));
          emit(enc::cmp(0, limit));
          emit(enc::csel(result, 0, limit, enc::ls));
        } else {
          emit(enc::movz(limit, 0x7fffU));
          emit(enc::cmp(0, limit));
          emit(enc::csel(result, 0, limit, enc::le));
          emit(enc::movn(limit, 0x7fffU));
          emit(enc::cmp(result, limit));
          emit(enc::csel(result, result, limit, enc::ge));
        }
        emit(enc::cmp(result, 0));
        const auto done = make_label();
        branch_condition(done, enc::eq);
        emit(enc::and_i(status, 2, *enc::logical_immediate_field(0x80U)));
        emit(enc::orr(status, status, 1));
        emit(enc::orr_i(status, status, *enc::logical_immediate_field(1U)));
        emit(0xd51b'4420U | status); // msr fpsr, x11
        bind(done);
        emit(enc::mov(0, result));
        if (dp && !o.unsigned_)
          emit(enc::x::sxtw(0, 0));
      } else if (dp && !o.unsigned_) {
        emit(enc::x::sxtw(0, 0));
      }
      if (dp)
        emit(enc::vec::float_int(enc::vec::fmov_from_w, 1, true, scratch_result, 0));
      else
        fmov_from_w(scratch_result);
      fp_write(d, dp, scratch_result);
      return Flow::next;
    }
    // FixedToFP rounds to nearest whatever FPCR says, which the Standard
    // value does; an integer has no denormal or NaN to treat otherwise.
    set_fp_mode(true);
    fmov_to_w(source);
    if (o.size == 16)
      emit(o.unsigned_ ? enc::uxth(0, 0) : enc::sxth(0, 0));
    emit(fraction == 0U
             ? enc::vec::float_int(o.unsigned_ ? enc::vec::ucvtf_w
                                               : enc::vec::scvtf_w,
                                   type, false, scratch_result, 0)
             : enc::vec::float_fixed(o.unsigned_ ? enc::vec::ucvtf_w
                                                 : enc::vec::scvtf_w,
                                     type, fraction, scratch_result, 0));
    fp_write(d, dp, scratch_result);
    return Flow::next;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Transfers.

A32Compiler::Flow A32Compiler::emit_vfp_move(const VInfo &v) {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  switch (v.variant) {
  case Variant::tos:
  case Variant::s: {
    // VMOV Rt, Sn / VMOV Sn, Rt.
    const unsigned s = static_cast<unsigned>(o.n);
    if (o.to_arm_register) {
      if (const int shadow = s_shadow_host(s); shadow >= 0) {
        const auto dest = def(static_cast<unsigned>(o.t), false, fallback_n);
        emit(enc::vec::float_int(enc::vec::fmov_to_w, 0, false,
                                 dest.native.host,
                                 static_cast<unsigned>(shadow)));
        done(dest);
        return Flow::next;
      }
      const unsigned host = read_native_vector(s >> 2, scratch_n).host;
      const auto dest = def(static_cast<unsigned>(o.t), false, fallback_n);
      emit(enc::vec::umov(2, dest.native.host, host, s & 3U));
      done(dest);
    } else {
      const unsigned source = use(static_cast<unsigned>(o.t), fallback_n);
      forget_s_shadow(s);
      if (s_shadowing_)
        emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false,
                                 take_s_shadow(s), source));
      const auto destination = q_def(s >> 2, true, s_shadowing_ ? 6U : 2U);
      emit(enc::vec::ins_general(2, destination.host, s & 3U, source));
      q_done(destination, s >> 2);
    }
    return Flow::next;
  }
  case Variant::tod:
  case Variant::d: {
    // VMOV Rt, Rt2, Dm / VMOV Dm, Rt, Rt2.
    const unsigned dm = static_cast<unsigned>(o.m);
    if (o.to_arm_registers) {
      const unsigned host = read_native_vector(dm >> 1, scratch_n).host;
      emit(enc::vec::umov(3, 0, host, dm & 1U));
      const auto low = def(static_cast<unsigned>(o.t), false, fallback_n);
      emit(enc::mov(low.native.host, 0));
      done(low);
      const auto high = def(static_cast<unsigned>(o.t2), false, fallback_n);
      emit(enc::x::lsr_i(high.native.host, 0, 32));
      done(high);
    } else {
      const unsigned first = use(static_cast<unsigned>(o.t), fallback_n);
      const unsigned second = use(static_cast<unsigned>(o.t2), fallback_m);
      emit(enc::x::orr(0, first, second, enc::lsl, 32));
      const auto destination = q_def(dm >> 1, true, 2);
      emit(enc::vec::ins_general(3, destination.host, dm & 1U, 0));
      q_done(destination, dm >> 1);
    }
    return Flow::next;
  }
  case Variant::toss:
  case Variant::ss: {
    // VMOV Rt, Rt2, Sm, Sm1 / VMOV Sm, Sm1, Rt, Rt2.
    const unsigned s0 = static_cast<unsigned>(o.m);
    const unsigned s1 = static_cast<unsigned>(o.m2);
    if (o.to_arm_registers) {
      const unsigned h0 = read_native_vector(s0 >> 2, scratch_n).host;
      const auto first = def(static_cast<unsigned>(o.t), false, fallback_n);
      emit(enc::vec::umov(2, first.native.host, h0, s0 & 3U));
      done(first);
      const unsigned h1 = read_native_vector(s1 >> 2, scratch_n).host;
      const auto second = def(static_cast<unsigned>(o.t2), false, fallback_n);
      emit(enc::vec::umov(2, second.native.host, h1, s1 & 3U));
      done(second);
    } else {
      const unsigned first = use(static_cast<unsigned>(o.t), fallback_n);
      {
        const auto destination = q_def(s0 >> 2, true, 2);
        emit(enc::vec::ins_general(2, destination.host, s0 & 3U, first));
        q_done(destination, s0 >> 2);
      }
      const unsigned second = use(static_cast<unsigned>(o.t2), fallback_m);
      const auto destination = q_def(s1 >> 2, true, 2);
      emit(enc::vec::ins_general(2, destination.host, s1 & 3U, second));
      q_done(destination, s1 >> 2);
    }
    return Flow::next;
  }
  case Variant::rs: {
    // VMOV.<size> Dd[x], Rt.
    const unsigned d = static_cast<unsigned>(o.d);
    const unsigned size = static_cast<unsigned>(o.lsb);
    const unsigned lanes = 8U >> size;
    const unsigned source = use(static_cast<unsigned>(o.t), fallback_n);
    const auto destination = q_def(d >> 1, true, 2);
    emit(enc::vec::ins_general(size, destination.host,
                               (d & 1U) * lanes +
                                   static_cast<unsigned>(o.index_value),
                               source));
    q_done(destination, d >> 1);
    return Flow::next;
  }
  case Variant::sr: {
    // VMOV.<type> Rt, Dn[x].
    const unsigned n = static_cast<unsigned>(o.n);
    const unsigned size = static_cast<unsigned>(o.lsb);
    const unsigned lanes = 8U >> size;
    const unsigned host = read_native_vector(n >> 1, scratch_n).host;
    const auto dest = def(static_cast<unsigned>(o.t), false, fallback_n);
    const unsigned index =
        (n & 1U) * lanes + static_cast<unsigned>(o.index_value);
    if (o.unsigned_ || size == 2U)
      emit(enc::vec::umov(size, dest.native.host, host, index));
    else
      emit(enc::vec::smov(size, dest.native.host, host, index));
    done(dest);
    return Flow::next;
  }
  default: {
    // VMOV (register) and VMOV (immediate), VFP.
    const bool dp = v.dp;
    const unsigned d = static_cast<unsigned>(o.d);
    if (v.variant == Variant::r) {
      const unsigned m = static_cast<unsigned>(o.m);
      if (m == d)
        return Flow::next;
      const unsigned source = fp_use(m, dp, scratch_m);
      fp_write(d, dp, source);
      return Flow::next;
    }
    if (v.variant == Variant::i) {
      const std::uint32_t word = inst.word;
      const unsigned imm8 = ((word >> 12) & 0xf0U) | (word & 0xfU);
      const unsigned result = fp_result(d, dp);
      emit(enc::vec::fmov_immediate(dp ? 1U : 0U, result, imm8));
      fp_write(d, dp, result);
      return Flow::next;
    }
    break;
  }
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// FPSCR.

A32Compiler::Flow A32Compiler::emit_fpscr(const VInfo &v) {
  const auto &o = cur().inst.operands;
  touch_fp_environment();
  const unsigned t = static_cast<unsigned>(o.t);
  if (v.mnemonic == VM::vmrs) {
    if (t == 15U) {
      // VMRS APSR_nzcv, FPSCR.
      if (flags_held_by_vmrs_ != count_) {
        emitter_.load_w(0, 15, offsetof(CpuState, a32_fpscr_nzcv));
        emit(enc::msr_nzcv(0));
      }
      store_flags();
      return Flow::next;
    }
    // FPSCR: the flags, QC and the cumulative bits, then the control bits.
    emitter_.load_w(0, 15, offsetof(CpuState, a32_fpscr_nzcv));
    emit(0xd53b'4421U); // mrs x1, fpsr
    emit(enc::movz(2, fpsr_writable_mask & 0xffffU));
    emit(enc::movk(2, fpsr_writable_mask >> 16, 1));
    emit(enc::and_(1, 1, 2));
    emit(enc::orr(0, 0, 1));
    emitter_.load_w(1, 15, offsetof(CpuState, fpcr));
    emit(enc::movz(2, aarch32::fpscr_control_mask >> 16, 1));
    emit(enc::and_(1, 1, 2));
    emit(enc::orr(0, 0, 1));
    const auto dest = def(t, false, fallback_n);
    emit(enc::mov(dest.native.host, 0));
    done(dest);
    return Flow::next;
  }
  // VMSR FPSCR, Rt. The host FPCR takes the written control bits, so the
  // translation continues in the guest's mode.
  const unsigned source = use(t, fallback_n);
  emit(enc::movz(1, aarch32::fpscr_control_mask >> 16, 1));
  emit(enc::and_(1, 1, source));
  emitter_.store_w(1, 15, offsetof(CpuState, fpcr));
  emit(0xd51b'4401U); // msr fpcr, x1
  emit(enc::movz(2, fpsr_writable_mask & 0xffffU));
  emit(enc::movk(2, fpsr_writable_mask >> 16, 1));
  emit(enc::and_(2, 2, source));
  emitter_.store_w(2, 15, offsetof(CpuState, fpsr));
  emit(0xd51b'4422U); // msr fpsr, x2
  emit(enc::and_i(0, source, *enc::logical_immediate_field(0xf000'0000U)));
  emitter_.store_w(0, 15, offsetof(CpuState, a32_fpscr_nzcv));
  ++guest_fp_mode_ops_;
  if (fp_mode_ != FpMode::guest) {
    emitter_.store_b(31, 15, offsetof(CpuState, a32_fp_standard));
    fp_mode_ = FpMode::guest;
  }
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
