// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/*
 * The A64 Advanced SIMD and floating-point encodings the AArch32 JIT writes
 * for VFP and NEON (jit/a32). Each function builds one 32-bit instruction
 * word from the fields of one A64 instruction class; the opcode enumerations
 * name the instructions the lowering uses, so a use reads as the mnemonic.
 * Register fields are host vector (or, where the instruction says so, general)
 * register numbers. arm64_jit_a32_vector_encoder_tests checks every
 * instruction class against the assembler.
 *
 * Arrangement fields: `size` is the A64 size field of the class (0 = 8-bit
 * lanes, 1 = 16, 2 = 32, 3 = 64) and `q` selects the 128-bit arrangement.
 */
namespace uml::cpu::arm64::jit::a32::enc::vec {

/* Three registers of the same arrangement: 0 Q U 01110 size 1 Rm opcode 1 Rn
 * Rd. The enumerators carry U in bit 5. */
enum Same : unsigned {
	shadd = 0x00, sqadd = 0x01, srhadd = 0x02, shsub = 0x04,
	sqsub = 0x05, cmgt = 0x06, cmge = 0x07, sshl = 0x08, sqshl = 0x09,
	srshl = 0x0a, sqrshl = 0x0b, smax = 0x0c, smin = 0x0d, sabd = 0x0e,
	saba = 0x0f, add = 0x10, cmtst = 0x11, mla = 0x12, mul = 0x13,
	smaxp = 0x14, sminp = 0x15, sqdmulh = 0x16, addp = 0x17,
	uhadd = 0x20, uqadd = 0x21, urhadd = 0x22, uhsub = 0x24,
	uqsub = 0x25, cmhi = 0x26, cmhs = 0x27, ushl = 0x28, uqshl = 0x29,
	urshl = 0x2a, uqrshl = 0x2b, umax = 0x2c, umin = 0x2d, uabd = 0x2e,
	uaba = 0x2f, sub = 0x30, cmeq = 0x31, mls = 0x32, pmul = 0x33,
	umaxp = 0x34, uminp = 0x35, sqrdmulh = 0x36,
};

[[nodiscard]] constexpr std::uint32_t same(bool q, Same op, unsigned size,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e20'0400U | (q ? 1U << 30 : 0U) | ((op >> 5) << 29) |
	       (size << 22) | (m << 16) | ((op & 31U) << 11) | (n << 5) | d;
}

/* The bitwise group (size selects): AND, BIC, ORR, ORN with U = 0; EOR, BSL,
 * BIT, BIF with U = 1. */
enum Logical : unsigned {
	and_ = 0, bic = 1, orr = 2, orn = 3, eor = 4, bsl = 5, bit = 6, bif = 7,
};
[[nodiscard]] constexpr std::uint32_t bitwise_op(bool q, Logical op,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e20'1c00U | (q ? 1U << 30 : 0U) | ((op >> 2) << 29) |
	       ((op & 3U) << 22) | (m << 16) | (n << 5) | d;
}

/* Floating-point three same: 0 Q U 01110 a sz 1 Rm opcode 1 Rn Rd. The
 * enumerators carry a in bit 6 and U in bit 5; sz = 0 is single. */
enum FloatSame : unsigned {
	fmaxnm = 0x18, fmla = 0x19, fadd = 0x1a, fmulx = 0x1b, fcmeq = 0x1c,
	fmax = 0x1e, frecps = 0x1f,
	fminnm = 0x58, fmls = 0x59, fsub = 0x5a, fmin = 0x5e, frsqrts = 0x5f,
	fmaxnmp = 0x38, faddp = 0x3a, fmul = 0x3b, fcmge = 0x3c, facge = 0x3d,
	fmaxp = 0x3e, fdiv = 0x3f,
	fminnmp = 0x78, fabd = 0x7a, fcmgt = 0x7c, facgt = 0x7d, fminp = 0x7e,
};
[[nodiscard]] constexpr std::uint32_t float_same(bool q, FloatSame op,
	bool double_precision, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e20'0400U | (q ? 1U << 30 : 0U) | (((op >> 5) & 1U) << 29) |
	       (((op >> 6) & 1U) << 23) | (double_precision ? 1U << 22 : 0U) |
	       (m << 16) | ((op & 31U) << 11) | (n << 5) | d;
}

/* Three registers of different widths: 0 Q U 01110 size 1 Rm opcode 00 Rn Rd.
 * Q selects the "2" form that takes the upper half of a narrow source or
 * writes the upper half of a narrow destination. The enumerators carry U in
 * bit 4. */
enum Diff : unsigned {
	saddl = 0x0, saddw = 0x1, ssubl = 0x2, ssubw = 0x3, addhn = 0x4,
	sabal = 0x5, subhn = 0x6, sabdl = 0x7, smlal = 0x8, sqdmlal = 0x9,
	smlsl = 0xa, sqdmlsl = 0xb, smull = 0xc, sqdmull = 0xd, pmull = 0xe,
	uaddl = 0x10, uaddw = 0x11, usubl = 0x12, usubw = 0x13, raddhn = 0x14,
	uabal = 0x15, rsubhn = 0x16, uabdl = 0x17, umlal = 0x18, umlsl = 0x1a,
	umull = 0x1c,
};
[[nodiscard]] constexpr std::uint32_t diff(bool q, Diff op, unsigned size,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e20'0000U | (q ? 1U << 30 : 0U) | ((op >> 4) << 29) |
	       (size << 22) | (m << 16) | ((op & 15U) << 12) | (n << 5) | d;
}

/* Two registers, miscellaneous: 0 Q U 01110 size 10000 opcode 10 Rn Rd. The
 * enumerators carry U in bit 5. For the floating-point members size bit 1 is
 * the `a` bit and bit 0 selects double precision (see float_misc). */
enum Misc : unsigned {
	rev64 = 0x00, rev16 = 0x01, saddlp = 0x02, suqadd = 0x03, cls = 0x04,
	cnt = 0x05, sadalp = 0x06, sqabs = 0x07, cmgt0 = 0x08, cmeq0 = 0x09,
	cmlt0 = 0x0a, abs_ = 0x0b, xtn = 0x12, sqxtn = 0x14,
	rev32 = 0x20, uaddlp = 0x22, usqadd = 0x23, clz = 0x24, not_ = 0x25,
	uadalp = 0x26, sqneg = 0x27, cmge0 = 0x28, cmle0 = 0x29, neg = 0x2b,
	sqxtun = 0x32, shll = 0x33, uqxtn = 0x34,
	/* The 32-bit reciprocal estimates (size 2). */
	urecpe = 0x1c, ursqrte = 0x3c,
};
[[nodiscard]] constexpr std::uint32_t misc(bool q, Misc op, unsigned size,
	unsigned d, unsigned n) noexcept
{
	return 0x0e20'0800U | (q ? 1U << 30 : 0U) | ((op >> 5) << 29) |
	       (size << 22) | ((op & 31U) << 12) | (n << 5) | d;
}

/* Floating-point two-register miscellaneous. The enumerators carry a in bit
 * 6, U in bit 5 and the opcode in bits 4:0. */
enum FloatMisc : unsigned {
	fcmgt0 = 0x4c, fcmeq0 = 0x4d, fcmlt0 = 0x4e, fabs_ = 0x4f,
	frintn = 0x18, frintm = 0x19, fcvtns = 0x1a, fcvtms = 0x1b, fcvtas = 0x1c,
	scvtf = 0x1d, frintp = 0x58, frintz = 0x59, fcvtps = 0x5a, fcvtzs = 0x5b,
	frecpe = 0x5d,
	fcmge0 = 0x6c, fcmle0 = 0x6d, fneg = 0x6f, frinta = 0x38, frintx = 0x39,
	fcvtnu = 0x3a, fcvtmu = 0x3b, fcvtau = 0x3c, ucvtf = 0x3d, frinti = 0x79,
	fcvtpu = 0x7a, fcvtzu = 0x7b, frsqrte = 0x7d, fsqrt = 0x7f,
};
[[nodiscard]] constexpr std::uint32_t float_misc(bool q, FloatMisc op,
	bool double_precision, unsigned d, unsigned n) noexcept
{
	return 0x0e20'0800U | (q ? 1U << 30 : 0U) | (((op >> 5) & 1U) << 29) |
	       (((op >> 6) & 1U) << 23) | (double_precision ? 1U << 22 : 0U) |
	       ((op & 31U) << 12) | (n << 5) | d;
}
/* The scalar form, 01 U 11110: S n to S d (or D to D), as SCVTF S5, S4. */
[[nodiscard]] constexpr std::uint32_t float_misc_scalar(FloatMisc op,
	bool double_precision, unsigned d, unsigned n) noexcept
{
	return float_misc(true, op, double_precision, d, n) | 1U << 28;
}

/* FCVTN and FCVTL (half <-> single, single <-> double): size bit 0 selects
 * the wider type. */
[[nodiscard]] constexpr std::uint32_t fcvtn(bool q, bool to_single,
	unsigned d, unsigned n) noexcept
{
	return 0x0e21'6800U | (q ? 1U << 30 : 0U) | (to_single ? 1U << 22 : 0U) |
	       (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t fcvtl(bool q, bool from_single,
	unsigned d, unsigned n) noexcept
{
	return 0x0e21'7800U | (q ? 1U << 30 : 0U) | (from_single ? 1U << 22 : 0U) |
	       (n << 5) | d;
}

/* Shift by immediate: 0 Q U 011110 immh:immb opcode 1 Rn Rd. `immhb` is the
 * seven-bit immh:immb field. The enumerators carry U in bit 5. */
enum ShiftImm : unsigned {
	sshr = 0x00, ssra = 0x02, srshr = 0x04, srsra = 0x06, shl = 0x0a,
	sqshl_i = 0x0e, shrn = 0x10, rshrn = 0x11, sqshrn = 0x12, sqrshrn = 0x13,
	sshll = 0x14, scvtf_i = 0x1c, fcvtzs_i = 0x1f,
	ushr = 0x20, usra = 0x22, urshr = 0x24, ursra = 0x26, sri = 0x28,
	sli = 0x2a, sqshlu = 0x2c, uqshl_i = 0x2e, sqshrun = 0x30,
	sqrshrun = 0x31, uqshrn = 0x32, uqrshrn = 0x33, ushll = 0x34,
	ucvtf_i = 0x3c, fcvtzu_i = 0x3f,
};
[[nodiscard]] constexpr std::uint32_t shift_imm(bool q, ShiftImm op,
	unsigned immhb, unsigned d, unsigned n) noexcept
{
	return 0x0f00'0400U | (q ? 1U << 30 : 0U) | ((op >> 5) << 29) |
	       (immhb << 16) | ((op & 31U) << 11) | (n << 5) | d;
}

/* Vector by element: 0 Q U 01111 size L M Rm opcode H 0 Rn Rd. `m` is the
 * element register (0-31 for 32-bit elements, 0-15 for 16-bit ones) and
 * `index` the element number (0-7 for 16-bit, 0-3 for 32-bit, 0-1 for
 * 64-bit). The enumerators carry U in bit 4. */
enum Element : unsigned {
	elem_mla = 0x10, elem_fmla = 0x01, elem_smlal = 0x02, elem_sqdmlal = 0x03,
	elem_mls = 0x14, elem_fmls = 0x05, elem_smlsl = 0x06, elem_sqdmlsl = 0x07,
	elem_mul = 0x08, elem_fmul = 0x09, elem_smull = 0x0a, elem_sqdmull = 0x0b,
	elem_sqdmulh = 0x0c, elem_sqrdmulh = 0x0d,
	elem_umlal = 0x12, elem_umlsl = 0x16, elem_umull = 0x1a,
	elem_sqrdmlah = 0x1d, elem_sqrdmlsh = 0x1f,
};
[[nodiscard]] constexpr std::uint32_t element(bool q, Element op,
	unsigned size, unsigned d, unsigned n, unsigned m,
	unsigned index) noexcept
{
	// 16-bit: index = H:L:M with Rm 4 bits; 32-bit: H:L with Rm 5 bits;
	// 64-bit: H with Rm 5 bits.
	unsigned h, l, mbit;
	unsigned rm = m;
	if (size == 1U) {
		h = (index >> 2) & 1U; l = (index >> 1) & 1U; mbit = index & 1U;
		rm = m & 15U;
	} else if (size == 2U) {
		h = (index >> 1) & 1U; l = index & 1U; mbit = (m >> 4) & 1U;
		rm = m & 15U;
	} else {
		h = index & 1U; l = 0U; mbit = (m >> 4) & 1U; rm = m & 15U;
	}
	return 0x0f00'0000U | (q ? 1U << 30 : 0U) | ((op >> 4) << 29) |
	       (size << 22) | (l << 21) | (mbit << 20) | (rm << 16) |
	       ((op & 15U) << 12) | (h << 11) | (n << 5) | d;
}

/* The floating-point by-element forms: size is 2 (single) or 3 (double), and
 * the element register is 0-31 as above. */
[[nodiscard]] constexpr std::uint32_t float_element(bool q, Element op,
	bool double_precision, unsigned d, unsigned n, unsigned m,
	unsigned index) noexcept
{
	return element(q, op, double_precision ? 3U : 2U, d, n, m, index);
}

/* Copy class: DUP, INS, UMOV, SMOV. `imm5` encodes the element size in its
 * lowest set bit and the index above it. */
[[nodiscard]] constexpr unsigned imm5(unsigned size, unsigned index) noexcept
{
	return (index << (size + 1U)) | (1U << size);
}
/* DUP Vd.T, Vn.Ts[index] */
[[nodiscard]] constexpr std::uint32_t dup_element(bool q, unsigned size,
	unsigned d, unsigned n, unsigned index) noexcept
{
	return 0x0e00'0400U | (q ? 1U << 30 : 0U) | (imm5(size, index) << 16) |
	       (n << 5) | d;
}
/* DUP Vd.T, Wn (or Xn for 64-bit lanes) */
[[nodiscard]] constexpr std::uint32_t dup_general(bool q, unsigned size,
	unsigned d, unsigned n) noexcept
{
	return 0x0e00'0c00U | (q ? 1U << 30 : 0U) | (imm5(size, 0U) << 16) |
	       (n << 5) | d;
}
/* DUP Bd/Hd/Sd/Dd, Vn.Ts[index]: the scalar form, which zeroes the rest of
 * the destination. */
[[nodiscard]] constexpr std::uint32_t dup_scalar(unsigned size, unsigned d,
	unsigned n, unsigned index) noexcept
{
	return 0x5e00'0400U | (imm5(size, index) << 16) | (n << 5) | d;
}
/* INS Vd.Ts[index], Wn (Xn for 64-bit lanes) */
[[nodiscard]] constexpr std::uint32_t ins_general(unsigned size, unsigned d,
	unsigned index, unsigned n) noexcept
{
	return 0x4e00'1c00U | (imm5(size, index) << 16) | (n << 5) | d;
}
/* INS Vd.Ts[dst_index], Vn.Ts[src_index] */
[[nodiscard]] constexpr std::uint32_t ins_element(unsigned size, unsigned d,
	unsigned dst_index, unsigned n, unsigned src_index) noexcept
{
	return 0x6e00'0400U | (imm5(size, dst_index) << 16) |
	       (src_index << (11U + size)) | (n << 5) | d;
}
/* UMOV Wd, Vn.Ts[index] (Xd for 64-bit lanes) */
[[nodiscard]] constexpr std::uint32_t umov(unsigned size, unsigned d,
	unsigned n, unsigned index) noexcept
{
	return 0x0e00'3c00U | (size == 3U ? 1U << 30 : 0U) |
	       (imm5(size, index) << 16) | (n << 5) | d;
}
/* SMOV Wd, Vn.Ts[index] (sign-extended to 32 bits) */
[[nodiscard]] constexpr std::uint32_t smov(unsigned size, unsigned d,
	unsigned n, unsigned index) noexcept
{
	return 0x0e00'2c00U | (imm5(size, index) << 16) | (n << 5) | d;
}

/* Permute: 0 Q 001110 size 0 Rm 0 opcode 10 Rn Rd. */
enum Permute : unsigned {
	uzp1 = 1, trn1 = 2, zip1 = 3, uzp2 = 5, trn2 = 6, zip2 = 7,
};
[[nodiscard]] constexpr std::uint32_t permute(bool q, Permute op,
	unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e00'0800U | (q ? 1U << 30 : 0U) | (size << 22) | (m << 16) |
	       (op << 12) | (n << 5) | d;
}
/* EXT Vd.T, Vn.T, Vm.T, #index (bytes) */
[[nodiscard]] constexpr std::uint32_t ext(bool q, unsigned d, unsigned n,
	unsigned m, unsigned index) noexcept
{
	return 0x2e00'0000U | (q ? 1U << 30 : 0U) | (m << 16) | (index << 11) |
	       (n << 5) | d;
}
/* TBL (extension = false) and TBX, with a table of 1-4 registers. */
[[nodiscard]] constexpr std::uint32_t table(bool q, bool extension,
	unsigned registers, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e00'0000U | (q ? 1U << 30 : 0U) | (m << 16) |
	       ((registers - 1U) << 13) | (extension ? 1U << 12 : 0U) |
	       (n << 5) | d;
}

/* Modified immediate: MOVI, MVNI, ORR, BIC and FMOV (vector). `op` and
 * `cmode` are the instruction's fields and `imm8` the eight-bit immediate. */
[[nodiscard]] constexpr std::uint32_t modified_immediate(bool q, unsigned op,
	unsigned cmode, unsigned imm8, unsigned d) noexcept
{
	return 0x0f00'0400U | (q ? 1U << 30 : 0U) | (op << 29) |
	       ((imm8 >> 5) << 16) | (cmode << 12) | ((imm8 & 31U) << 5) | d;
}

/* Scalar floating point: type 0 = single, 1 = double, 3 = half. */
/* One source: 0 0 0 11110 type 1 opcode 10000 Rn Rd. */
enum Float1 : unsigned {
	fmov = 0, fabs_s = 1, fneg_s = 2, fsqrt_s = 3,
	frintn_s = 8, frintp_s = 9, frintm_s = 10, frintz_s = 11, frinta_s = 12,
	frintx_s = 14, frinti_s = 15,
};
[[nodiscard]] constexpr std::uint32_t float1(Float1 op, unsigned type,
	unsigned d, unsigned n) noexcept
{
	return 0x1e20'4000U | (type << 22) | (op << 15) | (n << 5) | d;
}
/* FCVT: between half (3), single (0) and double (1). */
[[nodiscard]] constexpr std::uint32_t fcvt(unsigned to_type,
	unsigned from_type, unsigned d, unsigned n) noexcept
{
	return 0x1e22'4000U | (from_type << 22) | (to_type << 15) | (n << 5) | d;
}
/* Two sources: 0 0 0 11110 type 1 Rm opcode 10 Rn Rd. */
enum Float2 : unsigned {
	fmul_s = 0, fdiv_s = 1, fadd_s = 2, fsub_s = 3, fmax_s = 4, fmin_s = 5,
	fmaxnm_s = 6, fminnm_s = 7, fnmul_s = 8,
};
[[nodiscard]] constexpr std::uint32_t float2(Float2 op, unsigned type,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x1e20'0800U | (type << 22) | (m << 16) | (op << 12) | (n << 5) | d;
}
/* Three sources: 0 0 0 11111 type o1 Rm o0 Ra Rn Rd. */
enum Float3 : unsigned { fmadd = 0, fmsub = 1, fnmadd = 2, fnmsub = 3 };
[[nodiscard]] constexpr std::uint32_t float3(Float3 op, unsigned type,
	unsigned d, unsigned n, unsigned m, unsigned a) noexcept
{
	return 0x1f00'0000U | (type << 22) | ((op >> 1) << 21) | (m << 16) |
	       ((op & 1U) << 15) | (a << 10) | (n << 5) | d;
}
/* FCMP and FCMPE, register or against zero. */
[[nodiscard]] constexpr std::uint32_t fcmp(unsigned type, unsigned n,
	unsigned m, bool signalling, bool with_zero) noexcept
{
	return 0x1e20'2000U | (type << 22) | (m << 16) | (n << 5) |
	       (signalling ? 16U : 0U) | (with_zero ? 8U : 0U);
}
/* FMOV (scalar, immediate). */
[[nodiscard]] constexpr std::uint32_t fmov_immediate(unsigned type,
	unsigned d, unsigned imm8) noexcept
{
	return 0x1e20'1000U | (type << 22) | (imm8 << 13) | d;
}
/* FCSEL: Rd = condition ? Rn : Rm, on type 0 (single), 1 (double) or 3
 * (half). */
[[nodiscard]] constexpr std::uint32_t fcsel(unsigned type, unsigned d,
	unsigned n, unsigned m, unsigned condition) noexcept
{
	return 0x1e20'0c00U | (type << 22) | (m << 16) | (condition << 12) |
	       (n << 5) | d;
}
/* FJCVTZS Wd, Dn. */
[[nodiscard]] constexpr std::uint32_t fjcvtzs(unsigned d, unsigned n) noexcept
{
	return 0x1e7e'0000U | (n << 5) | d;
}
/* The cryptographic extension: AESE, AESD, AESMC and AESIMC (0-3) on
 * 16-byte blocks, and the SHA-1 and SHA-256 instructions of two and three
 * registers. */
enum Aes : unsigned { aese = 0x4e28'4800U, aesd = 0x4e28'5800U,
	aesmc = 0x4e28'6800U, aesimc = 0x4e28'7800U };
[[nodiscard]] constexpr std::uint32_t aes(Aes op, unsigned d, unsigned n) noexcept
{
	return op | (n << 5) | d;
}
enum Sha3 : unsigned { sha1c = 0x5e00'0000U, sha1p = 0x5e00'1000U,
	sha1m = 0x5e00'2000U, sha1su0 = 0x5e00'3000U, sha256h = 0x5e00'4000U,
	sha256h2 = 0x5e00'5000U, sha256su1 = 0x5e00'6000U };
[[nodiscard]] constexpr std::uint32_t sha3(Sha3 op, unsigned d, unsigned n,
	unsigned m) noexcept
{
	return op | (m << 16) | (n << 5) | d;
}
enum Sha2 : unsigned { sha1h = 0x5e28'0800U, sha1su1 = 0x5e28'1800U,
	sha256su0 = 0x5e28'2800U };
[[nodiscard]] constexpr std::uint32_t sha2(Sha2 op, unsigned d, unsigned n) noexcept
{
	return op | (n << 5) | d;
}

/* SDOT and UDOT, by vector: 0 Q U 01110 10 0 Rm 1001 01 Rn Rd. */
[[nodiscard]] constexpr std::uint32_t dot(bool q, bool is_unsigned,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x0e80'9400U | (q ? 1U << 30 : 0U) | (is_unsigned ? 1U << 29 : 0U) |
	       (m << 16) | (n << 5) | d;
}
/* SDOT and UDOT by element: 0 Q U 01111 10 L M Rm 1110 H 0 Rn Rd. `index`
 * (0-3) names the group of four bytes of the 128-bit register `m` (0-31). */
[[nodiscard]] constexpr std::uint32_t dot_element(bool q, bool is_unsigned,
	unsigned d, unsigned n, unsigned m, unsigned index) noexcept
{
	return 0x0f80'e000U | (q ? 1U << 30 : 0U) | (is_unsigned ? 1U << 29 : 0U) |
	       ((index & 1U) << 21) | (((index >> 1) & 1U) << 11) | (m << 16) |
	       (n << 5) | d;
}
/* SQRDMLAH and SQRDMLSH, by vector: 0 Q 1 01110 size 0 Rm 1000 S 1 Rn Rd. */
[[nodiscard]] constexpr std::uint32_t rdm(bool q, bool subtract,
	unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x2e00'8400U | (q ? 1U << 30 : 0U) | (size << 22) |
	       (subtract ? 1U << 11 : 0U) | (m << 16) | (n << 5) | d;
}
/* FCMLA, by vector: 0 Q 1 01110 size 0 Rm 11 rot 1 Rn Rd, with size 1 for
 * half and 2 for single precision, and the rotation in quarter turns. */
[[nodiscard]] constexpr std::uint32_t fcmla(bool q, unsigned size,
	unsigned rotation, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x2e00'c400U | (q ? 1U << 30 : 0U) | (size << 22) | (m << 16) |
	       (rotation << 11) | (n << 5) | d;
}
/* FCMLA by element: 0 Q 1 01111 size L M Rm 0 rot 1 H 0 Rn Rd. `index`
 * names the pair of elements of the 128-bit register `m` (0-31): 0-3 for
 * half and 0-1 for single precision. */
[[nodiscard]] constexpr std::uint32_t fcmla_element(bool q, unsigned size,
	unsigned rotation, unsigned d, unsigned n, unsigned m,
	unsigned index) noexcept
{
	const unsigned h = size == 1U ? (index >> 1) & 1U : index & 1U;
	const unsigned l = size == 1U ? index & 1U : 0U;
	return 0x2f00'1000U | (q ? 1U << 30 : 0U) | (size << 22) | (l << 21) |
	       (m << 16) | (rotation << 13) | (h << 11) | (n << 5) | d;
}
/* FCADD: 0 Q 1 01110 size 0 Rm 111 rot 01 Rn Rd, the rotation 0 for 90
 * degrees and 1 for 270. */
[[nodiscard]] constexpr std::uint32_t fcadd(bool q, unsigned size,
	unsigned rotation, unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x2e00'e400U | (q ? 1U << 30 : 0U) | (size << 22) | (m << 16) |
	       (rotation << 12) | (n << 5) | d;
}

/* Conversions between a general register and floating point (integer, sf =
 * 0 for W). rmode:opcode as the class defines. */
enum FloatInt : unsigned {
	fcvtzs_w = 0x18, fcvtzu_w = 0x19, scvtf_w = 0x02, ucvtf_w = 0x03,
	fmov_to_w = 0x06, fmov_from_w = 0x07,
	fcvtns_w = 0x00, fcvtnu_w = 0x01, fcvtps_w = 0x08, fcvtpu_w = 0x09,
	fcvtms_w = 0x10, fcvtmu_w = 0x11, fcvtas_w = 0x04, fcvtau_w = 0x05,
};
[[nodiscard]] constexpr std::uint32_t float_int(FloatInt op, unsigned type,
	bool x_register, unsigned d, unsigned n) noexcept
{
	return 0x1e20'0000U | (x_register ? 1U << 31 : 0U) | (type << 22) |
	       ((op >> 3) << 19) | ((op & 7U) << 16) | (n << 5) | d;
}
/* Fixed-point conversions (W register): scale = 64 - fractional bits. */
[[nodiscard]] constexpr std::uint32_t float_fixed(FloatInt op, unsigned type,
	unsigned fractional_bits, unsigned d, unsigned n) noexcept
{
	return 0x1e00'0000U | (type << 22) | ((op >> 3) << 19) |
	       ((op & 7U) << 16) | ((64U - fractional_bits) << 10) | (n << 5) | d;
}

/* Loads and stores. LDUR/STUR of a SIMD&FP register: the unscaled form,
 * signed offset -256..255 in bytes whatever the size (2 = S, 3 = D, 4 = Q). */
[[nodiscard]] constexpr std::uint32_t ldur_simd(unsigned log2_size,
	unsigned t, unsigned n, int offset) noexcept
{
	const unsigned size = log2_size == 4U ? 0U : log2_size;
	const unsigned opc = log2_size == 4U ? 3U : 1U;
	return 0x3c00'0000U | (size << 30) | (opc << 22) |
	       ((static_cast<unsigned>(offset) & 0x1ffU) << 12) | (n << 5) | t;
}
/* An LDUR or STUR at offset 0 as the register-offset form that adds the
 * zero-extended W index m: [Xn, Wm, uxtw]. */
[[nodiscard]] constexpr std::uint32_t indexed_uxtw(std::uint32_t transfer,
	unsigned m) noexcept
{
	return transfer | 0x0020'4800U | (m << 16);
}
[[nodiscard]] constexpr std::uint32_t stur_simd(unsigned log2_size,
	unsigned t, unsigned n, int offset) noexcept
{
	const unsigned size = log2_size == 4U ? 0U : log2_size;
	const unsigned opc = log2_size == 4U ? 2U : 0U;
	return 0x3c00'0000U | (size << 30) | (opc << 22) |
	       ((static_cast<unsigned>(offset) & 0x1ffU) << 12) | (n << 5) | t;
}
/* LDP/STP of Q registers, signed offset (a multiple of 16, -1024..1008). */
[[nodiscard]] constexpr std::uint32_t ldp_q(unsigned t, unsigned t2,
	unsigned n, int offset) noexcept
{
	return 0xad40'0000U | ((static_cast<unsigned>(offset / 16) & 0x7fU) << 15) |
	       (t2 << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t stp_q(unsigned t, unsigned t2,
	unsigned n, int offset) noexcept
{
	return 0xad00'0000U | ((static_cast<unsigned>(offset / 16) & 0x7fU) << 15) |
	       (t2 << 10) | (n << 5) | t;
}

/* Structure loads and stores, multiple structures: 0 Q 0011000 L 000000
 * opcode size Rn Rt. `structures` is 1-4 and `registers` (for one
 * structure) the number of registers 1-4. */
enum Structure : unsigned {
	ld1_1 = 0x7, ld1_2 = 0xa, ld1_3 = 0x6, ld1_4 = 0x2,
	ld2 = 0x8, ld3 = 0x4, ld4 = 0x0,
};
[[nodiscard]] constexpr std::uint32_t structure_multiple(bool q, bool load,
	Structure op, unsigned size, unsigned t, unsigned n) noexcept
{
	return 0x0c00'0000U | (q ? 1U << 30 : 0U) | (load ? 1U << 22 : 0U) |
	       (op << 12) | (size << 10) | (n << 5) | t;
}
/* Single structure to or from one lane, and the replicating loads:
 * 0 Q 0011010 L R 00000 opcode S size Rn Rt. `selem` is 1-4; the lane index
 * is for the given size. */
[[nodiscard]] constexpr std::uint32_t structure_lane(bool load, unsigned selem,
	unsigned size, unsigned lane, unsigned t, unsigned n) noexcept
{
	const unsigned r = (selem & 1U) == 0U ? 1U : 0U;
	const unsigned odd = selem >= 3U ? 1U : 0U;
	unsigned opcode, s, sz, q;
	if (size == 0U) {
		opcode = (odd ? 1U : 0U) | 0U; // 000 / 001
		q = (lane >> 3) & 1U; s = (lane >> 2) & 1U; sz = lane & 3U;
	} else if (size == 1U) {
		opcode = 2U | odd;
		q = (lane >> 2) & 1U; s = (lane >> 1) & 1U; sz = (lane & 1U) << 1;
	} else if (size == 2U) {
		opcode = 4U | odd;
		q = (lane >> 1) & 1U; s = lane & 1U; sz = 0U;
	} else {
		opcode = 4U | odd;
		q = lane & 1U; s = 0U; sz = 1U;
	}
	return 0x0d00'0000U | (q << 30) | (load ? 1U << 22 : 0U) | (r << 21) |
	       (opcode << 13) | (s << 12) | (sz << 10) | (n << 5) | t;
}
/* LDnR: replicate to all lanes. */
[[nodiscard]] constexpr std::uint32_t structure_replicate(bool q, unsigned selem,
	unsigned size, unsigned t, unsigned n) noexcept
{
	const unsigned r = (selem & 1U) == 0U ? 1U : 0U;
	const unsigned odd = selem >= 3U ? 1U : 0U;
	return 0x0d40'c000U | (q ? 1U << 30 : 0U) | (r << 21) | (odd << 13) |
	       (size << 10) | (n << 5) | t;
}

} // namespace uml::cpu::arm64::jit::a32::enc::vec
