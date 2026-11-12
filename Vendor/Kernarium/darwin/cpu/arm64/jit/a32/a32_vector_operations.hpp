// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

/* The mnemonics of the VFP and Advanced SIMD instructions the compiler
 * lowers at the simd level, in the decoder's spelling lower-cased. The same
 * mnemonic names the NEON and the VFP flavor of an instruction; VInfo says
 * which. */
enum class VM : std::uint8_t {
  none, fldmdbx, fldmiax, fstmdbx, fstmiax, vaba, vabal, vabd, vabdl, vabs,
  vacge, vacgt, vadd, vaddhn, vaddl, vaddw, vand, vbic, vbif, vbit, vbsl,
  vceq, vcge, vcgt, vcle, vcls, vclt, vclz, vcmp, vcmpe, vcnt, vcvt, vcvtb,
  vcvtr, vcvtt, vdiv, vdup, veor, vext, vfma, vfms, vfnma, vfnms, vhadd,
  vhsub, vld1, vld2, vld3, vld4, vldmdb, vldm, vldr, vmax, vmin, vmla, vmlal,
  vmls, vmlsl, vmov, vmovl, vmovn, vmrs, vmsr, vmul, vmull, vmvn, vneg,
  vnmla, vnmls, vnmul, vorn, vorr, vpadal, vpadd, vpaddl, vpmax, vpmin,
  vqabs, vqadd, vqdmlal, vqdmlsl, vqdmulh, vqdmull, vqmovn, vqmovun, vqneg,
  vqrdmulh, vqrshl, vqrshrn, vqrshrun, vqshl, vqshlu, vqshrn, vqshrun, vqsub,
  vraddhn, vrecpe, vrecps, vrev16, vrev32, vrev64, vrhadd, vrshl, vrshr,
  vrshrn, vrsqrte, vrsqrts, vrsra, vrsubhn, vshl, vshll, vshr, vshrn, vsli,
  vsqrt, vsra, vsri, vst1, vst2, vst3, vst4, vstmdb, vstm, vstr, vsub,
  vsubhn, vsubl, vsubw, vswp, vtbl, vtbx, vtrn, vtst, vuzp, vzip,
};

/* The second token of a form's name, which tells apart the encodings of one
 * mnemonic (VADD_f_A1_D is the NEON floating-point add, VMOV_tos_A1 moves a
 * core register to a single-precision register). */
enum class Variant : std::uint8_t {
  none, a, d, ds, f, hs, i, is, l, m, one, r, rs, s, sd, sh, siv, sr, ss,
  tod, tos, toss, toxv, uiv, vi, xs, xv,
};

/* How the write-back of a structure load or store is spelled. */
enum class Writeback : std::uint8_t { none, no_writeback, immediate, reg };

struct VInfo final {
  VM mnemonic{VM::none};
  Variant variant{Variant::none};
  Writeback writeback{Writeback::none};
  /* The VFP flavor (a scalar S or D register operation) rather than NEON. */
  bool vfp{};
  /* VFP: double precision. NEON: the form name's D/Q suffix is Q. */
  bool dp{};
  bool quad{};
  /* The encoding digit of the form name (A1 and T1 are 1). */
  std::uint8_t encoding{};
};

[[nodiscard]] const VInfo &vector_info(aarch32::Form form) noexcept;

/* Whether the mnemonic transfers vector registers to or from memory, and
 * whether it stores. */
[[nodiscard]] bool vector_memory(VM mnemonic) noexcept;
[[nodiscard]] bool vector_store(VM mnemonic) noexcept;

} // namespace uml::cpu::arm64::jit::a32
