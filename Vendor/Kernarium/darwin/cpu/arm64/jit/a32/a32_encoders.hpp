// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <cstdint>
#include <optional>

/*
 * The A64 encodings the AArch32 JIT writes (jit/a32): general-register data
 * processing on W registers, bitfield moves, multiplies, conditional selects,
 * flag manipulation, loads and stores, and the AdvSIMD forms the media
 * instructions use. Every function builds one 32-bit instruction word; the
 * register fields are host register numbers, 31 meaning ZR or SP as the
 * instruction defines it. arm64_jit_a32_encoder_tests checks the words
 * against the assembler.
 */
namespace uml::cpu::arm64::jit::a32::enc {

/* Condition codes (the A32 condition field is the same encoding). */
inline constexpr unsigned eq = 0, ne = 1, hs = 2, lo = 3, mi = 4, pl = 5,
	vs = 6, vc = 7, hi = 8, ls = 9, ge = 10, lt = 11, gt = 12, le = 13,
	al = 14;

enum Shift : unsigned { lsl = 0, lsr = 1, asr = 2, ror = 3 };

namespace detail {
[[nodiscard]] constexpr std::uint32_t three(std::uint32_t op, unsigned d,
	unsigned n, unsigned m) noexcept
{
	return op | (m << 16) | (n << 5) | d;
}
} // namespace detail

/* Shifted-register data processing, W. */
[[nodiscard]] constexpr std::uint32_t add(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x0b00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t adds(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x2b00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sub(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x4b00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t subs(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x6b00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t and_(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x0a00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t ands(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x6a00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t bic(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x0a20'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t bics(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x6a20'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t orr(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x2a00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t orn(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x2a20'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t eor(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x4a00'0000U | (s << 22) | (amount << 10), d, n, m);
}
/* MOV Wd, Wm */
[[nodiscard]] constexpr std::uint32_t mov(unsigned d, unsigned m) noexcept
{
	return orr(d, 31, m);
}
/* MVN Wd, Wm */
[[nodiscard]] constexpr std::uint32_t mvn(unsigned d, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return orn(d, 31, m, s, amount);
}
/* CMP Wn, Wm / CMN / TST */
[[nodiscard]] constexpr std::uint32_t cmp(unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return subs(31, n, m, s, amount);
}
[[nodiscard]] constexpr std::uint32_t cmn(unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return adds(31, n, m, s, amount);
}
[[nodiscard]] constexpr std::uint32_t tst(unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return ands(31, n, m, s, amount);
}

/* With carry. */
[[nodiscard]] constexpr std::uint32_t adc(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1a00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t adcs(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x3a00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sbc(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x5a00'0000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sbcs(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x7a00'0000U, d, n, m);
}

/* Add and subtract with a 12-bit immediate (optionally shifted left 12). */
[[nodiscard]] constexpr std::uint32_t add_i(unsigned d, unsigned n,
	unsigned imm12, bool shift12 = false) noexcept
{
	return 0x1100'0000U | (shift12 ? 1U << 22 : 0U) | (imm12 << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t adds_i(unsigned d, unsigned n,
	unsigned imm12, bool shift12 = false) noexcept
{
	return 0x3100'0000U | (shift12 ? 1U << 22 : 0U) | (imm12 << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t sub_i(unsigned d, unsigned n,
	unsigned imm12, bool shift12 = false) noexcept
{
	return 0x5100'0000U | (shift12 ? 1U << 22 : 0U) | (imm12 << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t subs_i(unsigned d, unsigned n,
	unsigned imm12, bool shift12 = false) noexcept
{
	return 0x7100'0000U | (shift12 ? 1U << 22 : 0U) | (imm12 << 10) | (n << 5) | d;
}

[[nodiscard]] constexpr std::uint32_t cmp_i(unsigned n, unsigned imm12) noexcept
{
	return subs_i(31, n, imm12);
}

/* The 13-bit field (N:immr:imms) of a 32-bit logical immediate, if the value
 * is one. N is always 0 for W. */
[[nodiscard]] constexpr std::optional<std::uint32_t> logical_immediate_field(
	std::uint32_t value) noexcept
{
	if (value == 0 || value == 0xffff'ffffU)
		return std::nullopt;
	/* Smallest repeating element. */
	unsigned size = 32;
	while (size > 2) {
		const unsigned half = size / 2;
		const std::uint32_t mask = (1U << half) - 1U;
		if ((value & mask) != ((value >> half) & mask))
			break;
		size = half;
	}
	const std::uint32_t mask = size == 32 ? 0xffff'ffffU : (1U << size) - 1U;
	const std::uint32_t element = value & mask;
	/* The element is a rotated run of ones. Rotate it so that the run starts
	 * at bit 0 and count the ones. */
	const unsigned ones = static_cast<unsigned>(std::popcount(element));
	if (ones == 0 || ones == size)
		return std::nullopt;
	unsigned rotation = 0;
	bool found = false;
	for (unsigned r = 0; r < size; ++r) {
		const std::uint32_t rotated = size == 32
			? std::rotr(element, static_cast<int>(r))
			: static_cast<std::uint32_t>(
				((element >> r) | (element << (size - r))) & mask);
		if (rotated == (1U << ones) - 1U) {
			rotation = r;
			found = true;
			break;
		}
	}
	if (!found)
		return std::nullopt;
	/* The element is ROR(run, rotation); immr rotates right by (size - ...). */
	const unsigned immr = (size - rotation) & (size - 1U);
	/* imms: the element size in the leading ones, then ones - 1. */
	const unsigned size_code = (~(size * 2U - 1U)) & 0x3fU;
	const unsigned imms = (size_code | (ones - 1U)) & 0x3fU;
	return (immr << 6) | imms;
}

[[nodiscard]] constexpr std::uint32_t and_i(unsigned d, unsigned n,
	std::uint32_t field) noexcept
{
	return 0x1200'0000U | ((field >> 6) << 16) | ((field & 0x3fU) << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t ands_i(unsigned d, unsigned n,
	std::uint32_t field) noexcept
{
	return 0x7200'0000U | ((field >> 6) << 16) | ((field & 0x3fU) << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t orr_i(unsigned d, unsigned n,
	std::uint32_t field) noexcept
{
	return 0x3200'0000U | ((field >> 6) << 16) | ((field & 0x3fU) << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t eor_i(unsigned d, unsigned n,
	std::uint32_t field) noexcept
{
	return 0x5200'0000U | ((field >> 6) << 16) | ((field & 0x3fU) << 10) | (n << 5) | d;
}

/* Move wide, W. */
[[nodiscard]] constexpr std::uint32_t movz(unsigned d, unsigned imm16,
	unsigned halfword = 0) noexcept
{
	return 0x5280'0000U | (halfword << 21) | (imm16 << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t movn(unsigned d, unsigned imm16,
	unsigned halfword = 0) noexcept
{
	return 0x1280'0000U | (halfword << 21) | (imm16 << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t movk(unsigned d, unsigned imm16,
	unsigned halfword = 0) noexcept
{
	return 0x7280'0000U | (halfword << 21) | (imm16 << 5) | d;
}

/* Bitfield moves, W, and their aliases. */
[[nodiscard]] constexpr std::uint32_t sbfm(unsigned d, unsigned n,
	unsigned immr, unsigned imms) noexcept
{
	return 0x1300'0000U | (immr << 16) | (imms << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t bfm(unsigned d, unsigned n,
	unsigned immr, unsigned imms) noexcept
{
	return 0x3300'0000U | (immr << 16) | (imms << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t ubfm(unsigned d, unsigned n,
	unsigned immr, unsigned imms) noexcept
{
	return 0x5300'0000U | (immr << 16) | (imms << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t lsl_i(unsigned d, unsigned n,
	unsigned amount) noexcept
{
	return ubfm(d, n, (32U - amount) & 31U, 31U - amount);
}
[[nodiscard]] constexpr std::uint32_t lsr_i(unsigned d, unsigned n,
	unsigned amount) noexcept
{
	return ubfm(d, n, amount, 31U);
}
[[nodiscard]] constexpr std::uint32_t asr_i(unsigned d, unsigned n,
	unsigned amount) noexcept
{
	return sbfm(d, n, amount, 31U);
}
[[nodiscard]] constexpr std::uint32_t ror_i(unsigned d, unsigned n,
	unsigned amount) noexcept
{
	/* EXTR Wd, Wn, Wn, #amount */
	return 0x1380'0000U | (n << 16) | (amount << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t extr(unsigned d, unsigned n,
	unsigned m, unsigned lsb) noexcept
{
	return 0x1380'0000U | (m << 16) | (lsb << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t uxtb(unsigned d, unsigned n) noexcept
{
	return ubfm(d, n, 0, 7);
}
[[nodiscard]] constexpr std::uint32_t uxth(unsigned d, unsigned n) noexcept
{
	return ubfm(d, n, 0, 15);
}
[[nodiscard]] constexpr std::uint32_t sxtb(unsigned d, unsigned n) noexcept
{
	return sbfm(d, n, 0, 7);
}
[[nodiscard]] constexpr std::uint32_t sxth(unsigned d, unsigned n) noexcept
{
	return sbfm(d, n, 0, 15);
}
/* UBFX / SBFX Wd, Wn, #lsb, #width */
[[nodiscard]] constexpr std::uint32_t ubfx(unsigned d, unsigned n,
	unsigned lsb, unsigned width) noexcept
{
	return ubfm(d, n, lsb, lsb + width - 1U);
}
[[nodiscard]] constexpr std::uint32_t sbfx(unsigned d, unsigned n,
	unsigned lsb, unsigned width) noexcept
{
	return sbfm(d, n, lsb, lsb + width - 1U);
}
/* BFI Wd, Wn, #lsb, #width */
[[nodiscard]] constexpr std::uint32_t bfi(unsigned d, unsigned n,
	unsigned lsb, unsigned width) noexcept
{
	return bfm(d, n, (32U - lsb) & 31U, width - 1U);
}
/* BFXIL Wd, Wn, #lsb, #width */
[[nodiscard]] constexpr std::uint32_t bfxil(unsigned d, unsigned n,
	unsigned lsb, unsigned width) noexcept
{
	return bfm(d, n, lsb, lsb + width - 1U);
}

/* Register-controlled shifts, W. */
[[nodiscard]] constexpr std::uint32_t lslv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'2000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t lsrv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'2400U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t asrv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'2800U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t rorv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'2c00U, d, n, m);
}

/* Multiply and divide. madd/msub are W; the long forms write X. */
[[nodiscard]] constexpr std::uint32_t madd(unsigned d, unsigned n, unsigned m,
	unsigned a) noexcept
{
	return 0x1b00'0000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t msub(unsigned d, unsigned n, unsigned m,
	unsigned a) noexcept
{
	return 0x1b00'8000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t smaddl(unsigned d, unsigned n,
	unsigned m, unsigned a) noexcept
{
	return 0x9b20'0000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t umaddl(unsigned d, unsigned n,
	unsigned m, unsigned a) noexcept
{
	return 0x9ba0'0000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t smsubl(unsigned d, unsigned n,
	unsigned m, unsigned a) noexcept
{
	return 0x9b20'8000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t mul(unsigned d, unsigned n, unsigned m) noexcept
{
	return madd(d, n, m, 31);
}
[[nodiscard]] constexpr std::uint32_t smull(unsigned d, unsigned n, unsigned m) noexcept
{
	return smaddl(d, n, m, 31);
}
[[nodiscard]] constexpr std::uint32_t umull(unsigned d, unsigned n, unsigned m) noexcept
{
	return umaddl(d, n, m, 31);
}
[[nodiscard]] constexpr std::uint32_t udiv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'0800U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sdiv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'0c00U, d, n, m);
}

/* CRC32 and CRC32C of the low 8, 16 or 32 bits of `m` into `n`: `size` is
 * 0, 1 or 2 for B, H or W. */
[[nodiscard]] constexpr std::uint32_t crc32(unsigned size, bool castagnoli,
	unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x1ac0'4000U | (castagnoli ? 1U << 12 : 0U) |
	                         (size << 10), d, n, m);
}

/* Bit manipulation, W. */
[[nodiscard]] constexpr std::uint32_t clz(unsigned d, unsigned n) noexcept
{
	return 0x5ac0'1000U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t rbit(unsigned d, unsigned n) noexcept
{
	return 0x5ac0'0000U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t rev(unsigned d, unsigned n) noexcept
{
	return 0x5ac0'0800U | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t rev16(unsigned d, unsigned n) noexcept
{
	return 0x5ac0'0400U | (n << 5) | d;
}

/* Conditional select and compare, W. */
[[nodiscard]] constexpr std::uint32_t csel(unsigned d, unsigned n, unsigned m,
	unsigned cond) noexcept
{
	return 0x1a80'0000U | (m << 16) | (cond << 12) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t csinc(unsigned d, unsigned n, unsigned m,
	unsigned cond) noexcept
{
	return 0x1a80'0400U | (m << 16) | (cond << 12) | (n << 5) | d;
}
/* CSET Wd, cond */
[[nodiscard]] constexpr std::uint32_t cset(unsigned d, unsigned cond) noexcept
{
	return csinc(d, 31, 31, cond ^ 1U);
}

/* The flags. */
[[nodiscard]] constexpr std::uint32_t msr_nzcv(unsigned n) noexcept
{
	return 0xd51b'4200U | n;
}
/* RMIF Xn, #rotate, #mask: inserts the flags (N Z C V in mask bits 3..0)
 * selected by mask from the low bits of Xn rotated right by `rotate`. */
[[nodiscard]] constexpr std::uint32_t rmif(unsigned n, unsigned rotate,
	unsigned mask) noexcept
{
	return 0xba00'0400U | (rotate << 15) | (n << 5) | mask;
}

/* Loads and stores with an unsigned scaled offset (byte offset given). */
[[nodiscard]] constexpr std::uint32_t ldr_w(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xb940'0000U | ((offset / 4U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t str_w(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xb900'0000U | ((offset / 4U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t ldr_x(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xf940'0000U | ((offset / 8U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t str_x(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xf900'0000U | ((offset / 8U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t ldrb(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x3940'0000U | (offset << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t strb(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x3900'0000U | (offset << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t ldrh(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x7940'0000U | ((offset / 2U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t strh(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x7900'0000U | ((offset / 2U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t ldrsb(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x39c0'0000U | (offset << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t ldrsh(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0x79c0'0000U | ((offset / 2U) << 10) | (n << 5) | t;
}
/* A load or store of the unsigned-offset forms above, at offset 0, turned
 * into the register-offset form that adds the zero-extended W index m:
 * [Xn, Wm, uxtw]. */
[[nodiscard]] constexpr std::uint32_t indexed_uxtw(std::uint32_t transfer,
	unsigned m) noexcept
{
	return (transfer & ~0x0100'0000U) | 0x0020'4800U | (m << 16);
}

/* Exclusives (the base register is a host address in Xn). size: 0 byte,
 * 1 half, 2 word, 3 doubleword. */
[[nodiscard]] constexpr std::uint32_t ldxr(unsigned size, unsigned t,
	unsigned n, bool acquire = false) noexcept
{
	return 0x085f'7c00U | (size << 30) | (acquire ? 0x8000U : 0U) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t stxr(unsigned size, unsigned s,
	unsigned t, unsigned n, bool release = false) noexcept
{
	return 0x0800'7c00U | (size << 30) | (s << 16) | (release ? 0x8000U : 0U)
		| (n << 5) | t;
}

/* Load-acquire and store-release (size as above). */
[[nodiscard]] constexpr std::uint32_t ldar(unsigned size, unsigned t,
	unsigned n) noexcept
{
	return 0x08df'fc00U | (size << 30) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t stlr(unsigned size, unsigned t,
	unsigned n) noexcept
{
	return 0x089f'fc00U | (size << 30) | (n << 5) | t;
}

[[nodiscard]] constexpr std::uint32_t nop() noexcept { return 0xd503'201fU; }
[[nodiscard]] constexpr std::uint32_t dmb_ish() noexcept { return 0xd503'3bbfU; }
[[nodiscard]] constexpr std::uint32_t dmb_ishst() noexcept { return 0xd503'3abfU; }
[[nodiscard]] constexpr std::uint32_t dsb_sy() noexcept { return 0xd503'3f9fU; }
[[nodiscard]] constexpr std::uint32_t isb() noexcept { return 0xd503'3fdfU; }

/* The X-register forms the compiler needs beside the W forms. */
namespace x {
[[nodiscard]] constexpr std::uint32_t add(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x8b00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sub(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0xcb00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t and_(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x8a00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t orr(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0xaa00'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t bic(unsigned d, unsigned n, unsigned m,
	Shift s = lsl, unsigned amount = 0) noexcept
{
	return detail::three(0x8a20'0000U | (s << 22) | (amount << 10), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t mov(unsigned d, unsigned m) noexcept
{
	return orr(d, 31, m);
}
/* TST Xn, Xm and CMP Xn, Xm. */
[[nodiscard]] constexpr std::uint32_t tst(unsigned n, unsigned m) noexcept
{
	return detail::three(0xea00'0000U, 31, n, m);
}
[[nodiscard]] constexpr std::uint32_t cmp(unsigned n, unsigned m) noexcept
{
	return detail::three(0xeb00'0000U, 31, n, m);
}
/* ADD Xd, Xn, Wm, SXTW #amount (0-4). */
[[nodiscard]] constexpr std::uint32_t add_sxtw(unsigned d, unsigned n,
	unsigned m, unsigned amount = 0) noexcept
{
	return 0x8b20'c000U | (m << 16) | (amount << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t lslv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x9ac0'2000U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t lsrv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x9ac0'2400U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t asrv(unsigned d, unsigned n, unsigned m) noexcept
{
	return detail::three(0x9ac0'2800U, d, n, m);
}
[[nodiscard]] constexpr std::uint32_t ubfm(unsigned d, unsigned n,
	unsigned immr, unsigned imms) noexcept
{
	return 0xd340'0000U | (immr << 16) | (imms << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t sbfm(unsigned d, unsigned n,
	unsigned immr, unsigned imms) noexcept
{
	return 0x9340'0000U | (immr << 16) | (imms << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t lsl_i(unsigned d, unsigned n, unsigned amount) noexcept
{
	return ubfm(d, n, (64U - amount) & 63U, 63U - amount);
}
[[nodiscard]] constexpr std::uint32_t lsr_i(unsigned d, unsigned n, unsigned amount) noexcept
{
	return ubfm(d, n, amount, 63U);
}
[[nodiscard]] constexpr std::uint32_t asr_i(unsigned d, unsigned n, unsigned amount) noexcept
{
	return sbfm(d, n, amount, 63U);
}
[[nodiscard]] constexpr std::uint32_t ubfx(unsigned d, unsigned n,
	unsigned lsb, unsigned width) noexcept
{
	return ubfm(d, n, lsb, lsb + width - 1U);
}
[[nodiscard]] constexpr std::uint32_t sxtw(unsigned d, unsigned n) noexcept
{
	return sbfm(d, n, 0, 31);
}
[[nodiscard]] constexpr std::uint32_t cmp_i(unsigned n, unsigned imm12) noexcept
{
	return 0xf100'001fU | (imm12 << 10) | (n << 5);
}
/* MOVZ Xd, #imm16, LSL #(16 * halfword) */
[[nodiscard]] constexpr std::uint32_t movz(unsigned d, unsigned imm16,
	unsigned halfword = 0) noexcept
{
	return 0xd280'0000U | (halfword << 21) | (imm16 << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t umull(unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x9ba0'7c00U | (m << 16) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t smull(unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x9b20'7c00U | (m << 16) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t mul(unsigned d, unsigned n, unsigned m) noexcept
{
	return 0x9b00'7c00U | (m << 16) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t madd(unsigned d, unsigned n, unsigned m,
	unsigned a) noexcept
{
	return 0x9b00'0000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t msub(unsigned d, unsigned n, unsigned m,
	unsigned a) noexcept
{
	return 0x9b00'8000U | (m << 16) | (a << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t ldr(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xf940'0000U | ((offset / 8U) << 10) | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t str(unsigned t, unsigned n,
	unsigned offset) noexcept
{
	return 0xf900'0000U | ((offset / 8U) << 10) | (n << 5) | t;
}
} // namespace x

/* AdvSIMD (the media instructions). Vd, Vn, Vm are host vector numbers. */
namespace v {
[[nodiscard]] constexpr std::uint32_t three_same(std::uint32_t op, unsigned d,
	unsigned n, unsigned m) noexcept
{
	return op | (m << 16) | (n << 5) | d;
}
/* Lane sizes: 0 = 8B, 1 = 4H, 2 = 2S for the 64-bit forms. */
[[nodiscard]] constexpr std::uint32_t add(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x0e20'8400U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sub(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x2e20'8400U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sqadd(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x0e20'0c00U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t uqadd(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x2e20'0c00U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t sqsub(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x0e20'2c00U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t uqsub(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x2e20'2c00U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t shadd(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x0e20'0400U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t uhadd(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x2e20'0400U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t shsub(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x0e20'2400U | (size << 22), d, n, m);
}
[[nodiscard]] constexpr std::uint32_t uhsub(unsigned size, unsigned d, unsigned n, unsigned m) noexcept
{
	return three_same(0x2e20'2400U | (size << 22), d, n, m);
}
} // namespace v

} // namespace uml::cpu::arm64::jit::a32::enc
