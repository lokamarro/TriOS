// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Armv8 extensions the compiler lowers: CRC32, the cryptographic
// instructions, VSEL, VMAXNM and VMINNM, VRINT, VCVT with a named rounding,
// VJCVT and SB. Each is the host's own instruction on the operands (their A32
// and A64 semantics are the same), under the guest's FPCR for the scalar
// forms and the Standard FPSCR value for the Advanced SIMD ones. The decoder
// presents a form only where the host has its extension (decode_profile), so
// the host instruction exists; a half-precision form and a half-precision
// Advanced SIMD variant leave through the semantic library.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

#include <string_view>

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr unsigned vector_n = 0;
constexpr unsigned vector_m = 1;
constexpr unsigned vector_result = 5;
constexpr unsigned vector_acc = 2;

/* The host's condition code for a VSEL mnemonic. */
constexpr unsigned vsel_condition(std::string_view mnemonic) noexcept {
  if (mnemonic == "VSELEQ")
    return 0U;
  if (mnemonic == "VSELVS")
    return 6U;
  if (mnemonic == "VSELGE")
    return 10U;
  return 12U; // VSELGT
}

} // namespace

A32Compiler::Flow A32Compiler::emit_extension(const OpInfo &) {
  const auto form = cur().inst.form;
  if (!extension_lowered(form))
    return emit_arith_through_helper();
  switch (extension_of(form)) {
  case Ext::crc32:
    return emit_crc32();
  case Ext::aes:
  case Ext::sha:
    return emit_crypto();
  case Ext::vsel:
    return emit_vsel();
  case Ext::vmaxnm:
    return emit_vmaxnm();
  case Ext::vrint:
    return emit_vrint();
  case Ext::vcvt_round:
    return emit_vcvt_round();
  case Ext::vjcvt:
    return emit_vjcvt();
  case Ext::dot:
    return emit_dot(false);
  case Ext::dot_element:
    return emit_dot(true);
  case Ext::rdm:
    return emit_rdm();
  case Ext::vcadd:
    return emit_vcadd();
  case Ext::vcmla:
    return emit_vcmla(false);
  case Ext::vcmla_element:
    return emit_vcmla(true);
  case Ext::sb:
    // The speculation barrier: DSB and ISB are a conforming implementation.
    emit(enc::dsb_sy());
    emit(enc::isb());
    return Flow::next;
  case Ext::none:
    break;
  }
  return emit_arith_through_helper();
}

// ---------------------------------------------------------------------------
// CRC32.

A32Compiler::Flow A32Compiler::emit_crc32() {
  const auto &o = cur().inst.operands;
  const unsigned n = use(static_cast<unsigned>(o.n), fallback_n);
  const unsigned m = use(static_cast<unsigned>(o.m), fallback_m);
  const auto dest = def(static_cast<unsigned>(o.d), false, fallback_n);
  const unsigned size = o.size == 8 ? 0U : o.size == 16 ? 1U : 2U;
  emit(enc::crc32(size, o.crc32c, dest.native.host, n, m));
  done(dest);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// AES and SHA: Q registers, whole.

A32Compiler::Flow A32Compiler::emit_crypto() {
  const auto &o = cur().inst.operands;
  const std::string_view mnemonic =
      aarch32::kForms[static_cast<std::size_t>(cur().inst.form)].mnemonic;
  touch_vectors();
  const unsigned d = static_cast<unsigned>(o.d) >> 1;
  const unsigned m = static_cast<unsigned>(o.m) >> 1;
  const auto two = [&](std::uint32_t word_base, bool reads_dest) {
    const unsigned source = q_use(m, vector_m);
    const auto dest = q_def(d, reads_dest, vector_result);
    emit(word_base | (source << 5) | dest.host);
    q_done(dest, d);
  };
  const auto three = [&](std::uint32_t word_base) {
    const unsigned first = q_use(static_cast<unsigned>(o.n) >> 1, vector_n);
    const unsigned second = q_use(m, vector_m);
    const auto dest = q_def(d, true, vector_result);
    emit(word_base | (second << 16) | (first << 5) | dest.host);
    q_done(dest, d);
  };
  if (mnemonic == "AESE")
    two(enc::vec::aese, true);
  else if (mnemonic == "AESD")
    two(enc::vec::aesd, true);
  else if (mnemonic == "AESMC")
    two(enc::vec::aesmc, false);
  else if (mnemonic == "AESIMC")
    two(enc::vec::aesimc, false);
  else if (mnemonic == "SHA1H")
    two(enc::vec::sha1h, false);
  else if (mnemonic == "SHA1SU1")
    two(enc::vec::sha1su1, true);
  else if (mnemonic == "SHA256SU0")
    two(enc::vec::sha256su0, true);
  else if (mnemonic == "SHA1C")
    three(enc::vec::sha1c);
  else if (mnemonic == "SHA1P")
    three(enc::vec::sha1p);
  else if (mnemonic == "SHA1M")
    three(enc::vec::sha1m);
  else if (mnemonic == "SHA1SU0")
    three(enc::vec::sha1su0);
  else if (mnemonic == "SHA256H")
    three(enc::vec::sha256h);
  else if (mnemonic == "SHA256H2")
    three(enc::vec::sha256h2);
  else if (mnemonic == "SHA256SU1")
    three(enc::vec::sha256su1);
  else
    return emit_arith_through_helper();
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VSEL.

A32Compiler::Flow A32Compiler::emit_vsel() {
  const auto &o = cur().inst.operands;
  const auto &info = aarch32::kForms[static_cast<std::size_t>(cur().inst.form)];
  const bool dp = std::string_view{info.name}.ends_with("_D");
  touch_vectors();
  load_flags();
  const unsigned left = fp_use(static_cast<unsigned>(o.n), dp, scratch_n);
  const unsigned right = fp_use(static_cast<unsigned>(o.m), dp, scratch_m);
  emit(enc::vec::fcsel(dp ? 1U : 0U, scratch_result, left, right,
                       vsel_condition(info.mnemonic)));
  fp_write(static_cast<unsigned>(o.d), dp, scratch_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VMAXNM and VMINNM.

A32Compiler::Flow A32Compiler::emit_vmaxnm() {
  const auto &o = cur().inst.operands;
  const auto &info = aarch32::kForms[static_cast<std::size_t>(cur().inst.form)];
  const std::string_view name = info.name;
  const bool maximum = std::string_view{info.mnemonic} == "VMAXNM";
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_fp_environment();
  if (name.ends_with("_D") && name.find("_A1_") == std::string_view::npos &&
      name.find("_T1_") == std::string_view::npos) {
    // VFP, double precision (the A2 and T2 forms).
    set_fp_mode(false);
    const unsigned left = fp_use(n, true, scratch_n);
    const unsigned right = fp_use(m, true, scratch_m);
    emit(enc::vec::float2(maximum ? enc::vec::fmaxnm_s : enc::vec::fminnm_s, 1,
                          scratch_result, left, right));
    fp_write(d, true, scratch_result);
    return Flow::next;
  }
  if (name.ends_with("_S")) {
    set_fp_mode(false);
    const unsigned left = fp_use(n, false, scratch_n);
    const unsigned right = fp_use(m, false, scratch_m);
    emit(enc::vec::float2(maximum ? enc::vec::fmaxnm_s : enc::vec::fminnm_s, 0,
                          scratch_result, left, right));
    fp_write(d, false, scratch_result);
    return Flow::next;
  }
  // Advanced SIMD: single precision under the Standard FPSCR value.
  if (o.esize == 16)
    return emit_arith_through_helper();
  const bool quad = name.ends_with("_Q");
  set_fp_mode(true);
  neon_op3(enc::vec::float_same(quad,
                                maximum ? enc::vec::fmaxnm : enc::vec::fminnm,
                                false, 0, 0, 0),
           quad, false, d, n, m, false);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VRINT.

A32Compiler::Flow A32Compiler::emit_vrint() {
  const auto &o = cur().inst.operands;
  const auto &info = aarch32::kForms[static_cast<std::size_t>(cur().inst.form)];
  const std::string_view name = info.name;
  const std::string_view mnemonic = info.mnemonic;
  const char kind = mnemonic.back(); // A, N, P, M, X, Z or R
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_fp_environment();
  if (name.find("_vfp_") != std::string_view::npos) {
    const bool dp = name.ends_with("_D");
    set_fp_mode(false);
    enc::vec::Float1 op = enc::vec::frintn_s;
    switch (kind) {
    case 'A': op = enc::vec::frinta_s; break;
    case 'N': op = enc::vec::frintn_s; break;
    case 'P': op = enc::vec::frintp_s; break;
    case 'M': op = enc::vec::frintm_s; break;
    case 'X': op = enc::vec::frintx_s; break;
    case 'Z': op = enc::vec::frintz_s; break;
    default: op = enc::vec::frinti_s; break; // VRINTR
    }
    const unsigned source = fp_use(m, dp, scratch_m);
    emit(enc::vec::float1(op, dp ? 1U : 0U, scratch_result, source));
    fp_write(d, dp, scratch_result);
    return Flow::next;
  }
  if (o.esize == 16)
    return emit_arith_through_helper();
  const bool quad = name.ends_with("_Q");
  enc::vec::FloatMisc op = enc::vec::frintn;
  switch (kind) {
  case 'A': op = enc::vec::frinta; break;
  case 'N': op = enc::vec::frintn; break;
  case 'P': op = enc::vec::frintp; break;
  case 'M': op = enc::vec::frintm; break;
  case 'X': op = enc::vec::frintx; break;
  default: op = enc::vec::frintz; break; // VRINTZ
  }
  set_fp_mode(true);
  neon_op2(enc::vec::float_misc(quad, op, false, 0, 0), quad, false, d, m,
           false);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VCVTA, VCVTN, VCVTP and VCVTM.

A32Compiler::Flow A32Compiler::emit_vcvt_round() {
  const auto &o = cur().inst.operands;
  const auto &info = aarch32::kForms[static_cast<std::size_t>(cur().inst.form)];
  const std::string_view name = info.name;
  const char kind = std::string_view{info.mnemonic}.back(); // A, N, P or M
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned m = static_cast<unsigned>(o.m);
  const bool is_unsigned = o.unsigned_;
  touch_fp_environment();
  if (name.find("_vfp_") != std::string_view::npos) {
    const bool dp = name.ends_with("_D");
    set_fp_mode(false);
    using enc::vec::FloatInt;
    FloatInt op = enc::vec::fcvtns_w;
    switch (kind) {
    case 'A': op = is_unsigned ? enc::vec::fcvtau_w : enc::vec::fcvtas_w; break;
    case 'N': op = is_unsigned ? enc::vec::fcvtnu_w : enc::vec::fcvtns_w; break;
    case 'P': op = is_unsigned ? enc::vec::fcvtpu_w : enc::vec::fcvtps_w; break;
    default: op = is_unsigned ? enc::vec::fcvtmu_w : enc::vec::fcvtms_w; break;
    }
    const unsigned source = fp_use(m, dp, scratch_m);
    emit(enc::vec::float_int(op, dp ? 1U : 0U, false, 0, source));
    emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false, scratch_result,
                             0));
    s_write(d, scratch_result);
    return Flow::next;
  }
  if (o.esize == 16)
    return emit_arith_through_helper();
  const bool quad = name.ends_with("_Q");
  using enc::vec::FloatMisc;
  FloatMisc op = enc::vec::fcvtns;
  switch (kind) {
  case 'A': op = is_unsigned ? enc::vec::fcvtau : enc::vec::fcvtas; break;
  case 'N': op = is_unsigned ? enc::vec::fcvtnu : enc::vec::fcvtns; break;
  case 'P': op = is_unsigned ? enc::vec::fcvtpu : enc::vec::fcvtps; break;
  default: op = is_unsigned ? enc::vec::fcvtmu : enc::vec::fcvtms; break;
  }
  set_fp_mode(true);
  neon_op2(enc::vec::float_misc(quad, op, false, 0, 0), quad, false, d, m,
           false);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VJCVT. FJCVTZS sets Z for an exact conversion, which is FPSCR's Z too. The
// hosts whose FJCVTZS sets Z for a flushed subnormal (the M1 family) get the
// A64 compiler's correction: a subnormal input is never exact.

A32Compiler::Flow A32Compiler::emit_vjcvt() {
  const auto &o = cur().inst.operands;
  touch_fp_environment();
  set_fp_mode(false);
  const unsigned source = fp_use(static_cast<unsigned>(o.m), true, scratch_m);
  clobber_flags();
  emit(enc::vec::fjcvtzs(0, source));
  if (!::uml::cpu::host_capabilities().jscvt_subnormal_flags_correct) {
    const auto indicator_done = make_label();
    // Scratch registers only: the host registers x3-x28 may hold the guest's.
    emit(0x9e66'0000U | (source << 5) | 9U);          // fmov x9, d<source>
    emit(0xd340'0000U | (52U << 16) | (62U << 10) | (9U << 5) | 10U); // ubfx x10, x9, #52, #11
    compare_zero(indicator_done, 10, true, true);
    emit(0xd340'0000U | (52U << 16) | (51U << 10) | (9U << 5) | 10U); // lsl x10, x9, #12
    compare_zero(indicator_done, 10, true, false);
    emit(0xd51b'421fU); // msr nzcv, xzr
    bind(indicator_done);
  }
  emit(0xd53b'4201U); // mrs x1, nzcv
  emitter_.store_w(1, 15, offsetof(CpuState, a32_fpscr_nzcv));
  emit(enc::vec::float_int(enc::vec::fmov_from_w, 0, false, scratch_result, 0));
  s_write(static_cast<unsigned>(o.d), scratch_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VSDOT and VUDOT. The element register is a D register of the 128-bit one
// the instruction names, so its group of four bytes is the (m & 1) * 2 + index
// group of that register.

A32Compiler::Flow A32Compiler::emit_dot(bool by_element) {
  const auto &o = cur().inst.operands;
  const bool quad = o.regs == 2;
  const bool is_unsigned = !o.signed_;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_vectors();
  if (!by_element) {
    neon_op3(enc::vec::dot(quad, is_unsigned, 0, 0, 0), quad, false, d, n, m,
             true);
    return Flow::next;
  }
  const unsigned index = (m & 1U) * 2U + static_cast<unsigned>(o.index_value);
  const unsigned scalar = q_use(m >> 1, vector_m);
  if (quad) {
    const unsigned first = q_use(n >> 1, vector_n);
    const auto dest = q_def(d >> 1, true, vector_result);
    emit(enc::vec::dot_element(true, is_unsigned, dest.host, first, scalar,
                               index));
    q_done(dest, d >> 1);
    return Flow::next;
  }
  const unsigned first = d_use(n, vector_n);
  vmov(vector_result, d_use(d, vector_acc));
  emit(enc::vec::dot_element(false, is_unsigned, vector_result, first, scalar,
                             index));
  d_write(d, vector_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VQRDMLAH and VQRDMLSH, by vector or by element. They saturate, so the block
// owns the native floating-point environment for QC.

A32Compiler::Flow A32Compiler::emit_rdm() {
  const auto &o = cur().inst.operands;
  const bool subtract = std::string_view{aarch32::kForms[static_cast<std::size_t>(
                            cur().inst.form)].mnemonic} == "VQRDMLSH";
  const bool quad = o.regs == 2;
  const unsigned size = esize_log2(o.esize);
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_vectors();
  touch_fp_environment();
  if (!o.scalar_form) {
    neon_op3(enc::vec::rdm(quad, subtract, size, 0, 0, 0), quad, false, d, n,
             m, true);
    return Flow::next;
  }
  const unsigned lanes = 64U / static_cast<unsigned>(o.esize);
  const unsigned index =
      (m & 1U) * lanes + static_cast<unsigned>(o.index_value);
  // The element register: a halfword element must be in V0-V15.
  unsigned scalar = q_use(m >> 1, vector_m);
  if (size == 1U && scalar >= 16U) {
    vmov(vector_m, scalar);
    scalar = vector_m;
  }
  const auto op = subtract ? enc::vec::elem_sqrdmlsh : enc::vec::elem_sqrdmlah;
  if (quad) {
    const unsigned first = q_use(n >> 1, vector_n);
    const auto dest = q_def(d >> 1, true, vector_result);
    emit(enc::vec::element(true, op, size, dest.host, first, scalar, index));
    q_done(dest, d >> 1);
    return Flow::next;
  }
  const unsigned first = d_use(n, vector_n);
  vmov(vector_result, d_use(d, vector_acc));
  emit(enc::vec::element(false, op, size, vector_result, first, scalar, index));
  d_write(d, vector_result);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// VCADD and VCMLA, single or half precision, under the Standard FPSCR value.

A32Compiler::Flow A32Compiler::emit_vcadd() {
  const auto &o = cur().inst.operands;
  const bool half = o.esize == 16;
  if (half && !::uml::cpu::host_capabilities().has(::uml::cpu::ArmFeature::fp16))
    return emit_arith_through_helper();
  const bool quad = o.regs == 2;
  const unsigned rotation = (cur().inst.word >> 24) & 1U;
  touch_vectors();
  touch_fp_environment();
  set_fp_mode(true);
  neon_op3(enc::vec::fcadd(quad, half ? 1U : 2U, rotation, 0, 0, 0), quad,
           false, static_cast<unsigned>(o.d), static_cast<unsigned>(o.n),
           static_cast<unsigned>(o.m), false);
  return Flow::next;
}

A32Compiler::Flow A32Compiler::emit_vcmla(bool by_element) {
  const auto &o = cur().inst.operands;
  const bool half = o.esize == 16;
  if (half && !::uml::cpu::host_capabilities().has(::uml::cpu::ArmFeature::fp16))
    return emit_arith_through_helper();
  const bool quad = o.regs == 2;
  const unsigned size = half ? 1U : 2U;
  const unsigned d = static_cast<unsigned>(o.d);
  const unsigned n = static_cast<unsigned>(o.n);
  const unsigned m = static_cast<unsigned>(o.m);
  touch_vectors();
  touch_fp_environment();
  set_fp_mode(true);
  if (!by_element) {
    const unsigned rotation = (cur().inst.word >> 23) & 3U;
    neon_op3(enc::vec::fcmla(quad, size, rotation, 0, 0, 0), quad, false, d, n,
             m, true);
    return Flow::next;
  }
  // The element is a pair of D(m); its place in the 128-bit register is
  // counted in pairs.
  const unsigned rotation = (cur().inst.word >> 20) & 3U;
  const unsigned pairs = half ? 2U : 1U;
  const unsigned index =
      (m & 1U) * pairs + static_cast<unsigned>(o.index_value);
  const unsigned scalar = q_use(m >> 1, vector_m);
  if (quad) {
    const unsigned first = q_use(n >> 1, vector_n);
    const auto dest = q_def(d >> 1, true, vector_result);
    emit(enc::vec::fcmla_element(true, size, rotation, dest.host, first,
                                 scalar, index));
    q_done(dest, d >> 1);
    return Flow::next;
  }
  if (half && index < 2U) {
    const unsigned first = d_use(n, vector_n);
    vmov(vector_result, d_use(d, vector_acc));
    emit(enc::vec::fcmla_element(false, size, rotation, vector_result, first,
                                 scalar, index));
    d_write(d, vector_result);
    return Flow::next;
  }
  // A64 has no 64-bit arrangement of the single-precision form, nor one of
  // the half-precision form for a pair in the upper half of the element
  // register: the 128-bit one runs on the doubleword in both halves.
  const unsigned first = d_use_twice(n, vector_n);
  vmov(vector_result, d_use_twice(d, vector_acc));
  emit(enc::vec::fcmla_element(true, size, rotation, vector_result, first,
                               scalar, index));
  d_write(d, vector_result);
  return Flow::next;
}

} // namespace uml::cpu::arm64::jit::a32
