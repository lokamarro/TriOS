/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x86 SIMD floating-point exactness. Each cell runs one register-form
 * instruction through the gadget engine, in a guest loop over a deterministic
 * operand stream, under one MXCSR (DAZ, FTZ and rounding control), and
 * compares every result lane with an x86 model built from correctly rounded
 * host arithmetic and the x86 rules the host does not share: first-source
 * NaN priority, the negative indefinite NaN, DAZ input and FTZ output
 * flushes with underflow detected after rounding, MIN and MAX returning the
 * second source, RCP and RSQRT treating denormal inputs as zero, FMA's
 * operand order, and comparisons under DAZ, COMIS's flags included. The
 * model's rules, and its MXCSR.DE, were checked against an Intel host. The
 * stream mixes uniform values, special values and exponents near the
 * underflow threshold. A second pass runs every cell with MXCSR unmasking a
 * denormal operand, so that each instruction runs checked: one x86 raises
 * MXCSR.DE for must fault with #XM and keep its destination, and every other
 * must match the model. The JIT runs a third pass against the gadget engine
 * as its reference, under MXCSRs that unmask some exceptions, over these
 * instructions and roundings, conversions from integers and half-precision
 * conversions: each instruction must fault in both or in neither, with the
 * same flags and its destination kept, or compute the same results, and
 * raise the same flags while MXCSR unmasks a denormal operand, underflow or
 * precision, which every check of both engines settles. `--iterations N`,
 * `--checked-iterations N` and `--reference-iterations N` set the loop
 * counts per cell; the defaults keep the test short. */
#include "x86_64_sse_loop.hpp"

#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/exception_checks.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using uml::cpu::x86_64::CpuState;
using uml::cpu::x86_64::ExecutionMode;
using uml::cpu::x86_64::Register;
using namespace uml::test::x86_64_sse_loop;

/* ---- host arithmetic: one correctly rounded instruction under the live
 * FPCR, whose rounding mode is the cell's and whose FZ, DN and AH are clear.
 */

class HostRounding final {
public:
	/* MXCSR numbers the directed modes the other way round from FPCR. */
	explicit HostRounding(unsigned rounding_control) noexcept
	{
		__asm__ volatile("mrs %0, fpcr" : "=r"(saved_));
		static constexpr std::array<std::uint64_t, 4> rmode{0, 2, 1, 3};
		const std::uint64_t fpcr = rmode[rounding_control & 3U] << 22U;
		__asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
	}
	~HostRounding() { __asm__ volatile("msr fpcr, %0" :: "r"(saved_)); }
	HostRounding(const HostRounding &) = delete;
	HostRounding &operator=(const HostRounding &) = delete;

private:
	std::uint64_t saved_{};
};

/* `reference`: an instruction only the JIT's reference pass runs. */
enum class Op : std::uint8_t {
	add, sub, mul, div, sqrt, min, max, rcp, rsqrt, fma, convert,
	hadd, hsub, addsub, dot, cmp, comis, reference,
};

[[nodiscard]] std::uint64_t host32(Op op, std::uint32_t a, std::uint32_t b)
{
	std::uint32_t r{};
	switch (op) {
	case Op::add:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fadd s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::sub:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fsub s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::mul:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmul s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::div:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fdiv s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	default:
		__asm__ volatile("fmov s1, %w1\n fsqrt s2, s1\n fmov %w0, s2"
			: "=r"(r) : "r"(b) : "v1", "v2");
		break;
	}
	return r;
}

[[nodiscard]] std::uint64_t host64(Op op, std::uint64_t a, std::uint64_t b)
{
	std::uint64_t r{};
	switch (op) {
	case Op::add:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fadd d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::sub:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fsub d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::mul:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fmul d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::div:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fdiv d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	default:
		__asm__ volatile("fmov d1, %1\n fsqrt d2, d1\n fmov %0, d2"
			: "=r"(r) : "r"(b) : "v1", "v2");
		break;
	}
	return r;
}

/* Ordered a < b. */
[[nodiscard]] bool host_less(bool dbl, std::uint64_t a, std::uint64_t b)
{
	std::uint32_t r{};
	if (dbl)
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fcmp d0, d1\n"
			" cset %w0, mi" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "cc");
	else
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fcmp s0, s1\n"
			" cset %w0, mi" : "=r"(r)
			: "r"(static_cast<std::uint32_t>(a)),
			  "r"(static_cast<std::uint32_t>(b)) : "v0", "v1", "cc");
	return r != 0;
}

[[nodiscard]] std::uint64_t host(bool dbl, Op op, std::uint64_t a,
	std::uint64_t b)
{
	return dbl ? host64(op, a, b) :
		host32(op, static_cast<std::uint32_t>(a),
			static_cast<std::uint32_t>(b));
}

/* a * b + c, fused. */
[[nodiscard]] std::uint64_t host_fma(bool dbl, std::uint64_t a,
	std::uint64_t b, std::uint64_t c)
{
	std::uint64_t r{};
	if (dbl)
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fmov d2, %3\n"
			" fmadd d3, d0, d1, d2\n fmov %0, d3" : "=r"(r)
			: "r"(a), "r"(b), "r"(c) : "v0", "v1", "v2", "v3");
	else {
		std::uint32_t narrow{};
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmov s2, %w3\n"
			" fmadd s3, s0, s1, s2\n fmov %w0, s3" : "=r"(narrow)
			: "r"(static_cast<std::uint32_t>(a)),
			  "r"(static_cast<std::uint32_t>(b)),
			  "r"(static_cast<std::uint32_t>(c)) : "v0", "v1", "v2", "v3");
		r = narrow;
	}
	return r;
}

[[nodiscard]] std::uint64_t host_widen(std::uint32_t single)
{
	std::uint64_t r{};
	__asm__ volatile("fmov s0, %w1\n fcvt d1, s0\n fmov %0, d1" : "=r"(r)
		: "r"(single) : "v0", "v1");
	return r;
}

[[nodiscard]] std::uint32_t host_narrow(std::uint64_t value)
{
	std::uint32_t r{};
	__asm__ volatile("fmov d0, %1\n fcvt s1, d0\n fmov %w0, s1" : "=r"(r)
		: "r"(value) : "v0", "v1");
	return r;
}

/* Rounded to an integral value in the live rounding mode, or toward zero. */
[[nodiscard]] std::uint64_t host_integral(bool dbl, std::uint64_t value,
	bool truncate)
{
	std::uint64_t r{};
	if (dbl && truncate)
		__asm__ volatile("fmov d0, %1\n frintz d1, d0\n fmov %0, d1"
			: "=r"(r) : "r"(value) : "v0", "v1");
	else if (dbl)
		__asm__ volatile("fmov d0, %1\n frinti d1, d0\n fmov %0, d1"
			: "=r"(r) : "r"(value) : "v0", "v1");
	else if (truncate)
		__asm__ volatile("fmov s0, %w1\n frintz s1, s0\n fmov %w0, s1"
			: "=r"(r) : "r"(static_cast<std::uint32_t>(value)) : "v0", "v1");
	else
		__asm__ volatile("fmov s0, %w1\n frinti s1, s0\n fmov %w0, s1"
			: "=r"(r) : "r"(static_cast<std::uint32_t>(value)) : "v0", "v1");
	return r;
}

/* ---- the x86 model ---- */

struct Format final {
	bool dbl;
	[[nodiscard]] std::uint64_t sign() const noexcept
	{
		return dbl ? 0x8000'0000'0000'0000ULL : 0x8000'0000ULL;
	}
	[[nodiscard]] std::uint64_t magnitude() const noexcept
	{
		return dbl ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL;
	}
	[[nodiscard]] std::uint64_t exponent() const noexcept
	{
		return dbl ? 0x7ff0'0000'0000'0000ULL : 0x7f80'0000ULL;
	}
	[[nodiscard]] std::uint64_t quiet_bit() const noexcept
	{
		return dbl ? 0x0008'0000'0000'0000ULL : 0x0040'0000ULL;
	}
	[[nodiscard]] std::uint64_t minimum_normal() const noexcept
	{
		return dbl ? 0x0010'0000'0000'0000ULL : 0x0080'0000ULL;
	}
	[[nodiscard]] std::uint64_t indefinite() const noexcept
	{
		return sign() | exponent() | quiet_bit();
	}
	[[nodiscard]] bool nan(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) > exponent();
	}
	[[nodiscard]] bool denormal(std::uint64_t x) const noexcept
	{
		return (x & exponent()) == 0 && (x & magnitude()) != 0;
	}
	[[nodiscard]] std::uint64_t flush(std::uint64_t x) const noexcept
	{
		return denormal(x) ? (x & sign()) : x;
	}
	[[nodiscard]] std::uint64_t quiet(std::uint64_t x) const noexcept
	{
		return x | quiet_bit();
	}
};

/* x86 detects underflow after rounding: a result is tiny when, rounded to
 * full precision with an unbounded exponent, it lies below the smallest
 * normal. A denormal result surely is; a product or quotient the host
 * rounded up to exactly the smallest normal is when the same operation,
 * scaled into the normal range, stays below its scaled threshold. Sums and
 * differences that small are exact. */
[[nodiscard]] bool tiny(Format f, Op op, std::uint64_t a, std::uint64_t b,
	std::uint64_t result)
{
	const auto magnitude = result & f.magnitude();
	if (f.denormal(result))
		return true;
	if (magnitude != f.minimum_normal() || (op != Op::mul && op != Op::div))
		return false;
	const int k = f.dbl ? 512 : 64;
	const std::uint64_t scale = f.dbl ?
		static_cast<std::uint64_t>(1023 + k) << 52U :
		static_cast<std::uint64_t>(127 + k) << 23U;
	const auto scaled = host(f.dbl, Op::mul, a, scale);
	const auto rescaled = host(f.dbl, op, scaled, b) & f.magnitude();
	const std::uint64_t threshold = f.dbl ?
		static_cast<std::uint64_t>(1 + k) << 52U :
		static_cast<std::uint64_t>(1 + k) << 23U;
	return rescaled < threshold;
}

/* One lane of an SSE arithmetic, minimum or maximum, square root,
 * reciprocal or reciprocal square root. `a` is the first source, `b` the
 * second (the only one of the unary operations). */
[[nodiscard]] std::uint64_t x86_lane(Format f, Op op, std::uint64_t a,
	std::uint64_t b, bool daz, bool ftz)
{
	if (op == Op::rcp || op == Op::rsqrt) {
		/* MXCSR does not govern them: a denormal input is zero, and a
		 * reciprocal of 2^126 or more is zero too. */
		b = f.flush(b);
		if (f.nan(b))
			return f.quiet(b);
		if (op == Op::rcp) {
			const auto magnitude = b & f.magnitude();
			if (magnitude >= 0x7e80'0000ULL && magnitude <= 0x7f80'0000ULL)
				return b & f.sign();
			return host(f.dbl, Op::div, f.dbl ?
				0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL, b);
		}
		if ((b & f.sign()) != 0 && (b & f.magnitude()) != 0)
			return f.indefinite();
		const auto root = host(f.dbl, Op::sqrt, 0, b);
		return host(f.dbl, Op::div, f.dbl ?
			0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL, root);
	}
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	if (op == Op::min || op == Op::max) {
		if (f.nan(a) || f.nan(b))
			return b;
		const bool take_first = op == Op::min ? host_less(f.dbl, a, b) :
			host_less(f.dbl, b, a);
		return take_first ? a : b;
	}
	if (op == Op::sqrt) {
		if (f.nan(b))
			return f.quiet(b);
	} else if (f.nan(a) || f.nan(b)) {
		return f.nan(a) ? f.quiet(a) : f.quiet(b);
	}
	const auto result = host(f.dbl, op, a, b);
	if (f.nan(result))
		return f.indefinite();
	if (ftz && tiny(f, op, a, b, result))
		return result & f.sign();
	return result;
}

/* The same for a * b + c, whose operands are already signed as the
 * operation negates them. A result the host rounded up to the smallest
 * normal is tiny when the operation with a and c scaled up stays below the
 * scaled threshold. */
[[nodiscard]] bool tiny_fused(Format f, std::uint64_t a, std::uint64_t b,
	std::uint64_t c, std::uint64_t result)
{
	if (f.denormal(result))
		return true;
	if ((result & f.magnitude()) != f.minimum_normal())
		return false;
	const int k = f.dbl ? 512 : 64;
	const std::uint64_t scale = f.dbl ?
		static_cast<std::uint64_t>(1023 + k) << 52U :
		static_cast<std::uint64_t>(127 + k) << 23U;
	const auto rescaled = host_fma(f.dbl, host(f.dbl, Op::mul, a, scale), b,
		host(f.dbl, Op::mul, c, scale)) & f.magnitude();
	const std::uint64_t threshold = f.dbl ?
		static_cast<std::uint64_t>(1 + k) << 52U :
		static_cast<std::uint64_t>(1 + k) << 23U;
	return rescaled < threshold;
}

/* One lane of an FMA: x86's a * b + c with the product and the addend
 * negated as `negate_product` and `negate_addend` say. A NaN result is the
 * first of a, b and c that is a NaN, quieted and with its own sign. */
[[nodiscard]] std::uint64_t x86_fused_lane(Format f, std::uint64_t a,
	std::uint64_t b, std::uint64_t c, bool negate_product,
	bool negate_addend, bool daz, bool ftz)
{
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
		c = f.flush(c);
	}
	for (const auto value : {a, b, c})
		if (f.nan(value))
			return f.quiet(value);
	const auto signed_a = negate_product ? a ^ f.sign() : a;
	const auto signed_c = negate_addend ? c ^ f.sign() : c;
	const auto result = host_fma(f.dbl, signed_a, b, signed_c);
	if (f.nan(result))
		return f.indefinite();
	if (ftz && tiny_fused(f, signed_a, b, signed_c, result))
		return result & f.sign();
	return result;
}

/* CVTSS2SD and CVTPS2PD: DAZ flushes the input; every single is a normal
 * double, and the host widens a NaN as x86 does. */
[[nodiscard]] std::uint64_t x86_widen(std::uint32_t value, bool daz)
{
	const Format single{false};
	if (daz)
		value = static_cast<std::uint32_t>(single.flush(value));
	return host_widen(value);
}

/* CVTSD2SS and CVTPD2PS: DAZ flushes the input and FTZ a result tiny after
 * rounding, which a value the host rounds up to the smallest normal is when
 * the same value scaled up rounds below the scaled threshold. */
[[nodiscard]] std::uint32_t x86_narrow(std::uint64_t value, bool daz,
	bool ftz)
{
	const Format single{false};
	const Format dbl{true};
	if (daz)
		value = dbl.flush(value);
	const std::uint32_t result = host_narrow(value);
	if (!ftz || single.nan(result))
		return result;
	const auto magnitude = result & single.magnitude();
	bool tiny = single.denormal(result);
	if (magnitude == single.minimum_normal()) {
		const auto scaled = host_narrow(host(true, Op::mul, value,
			static_cast<std::uint64_t>(1023 + 64) << 52U));
		tiny = (scaled & single.magnitude()) < (65U << 23U);
	}
	return tiny ? (result & 0x8000'0000U) : result;
}

/* CVT{,T}{SS,SD,PS,PD}2{SI,DQ}: DAZ flushes the input; a NaN or a value
 * out of range gives the integer indefinite. */
[[nodiscard]] std::uint64_t x86_to_integer(bool dbl, std::uint64_t value,
	unsigned bits, bool truncate, bool daz)
{
	const Format f{dbl};
	if (daz)
		value = f.flush(value);
	const std::uint64_t indefinite = 1ULL << (bits - 1U);
	if (f.nan(value))
		return indefinite;
	const auto integral = host_integral(dbl, value, truncate);
	double number{};
	if (dbl) {
		std::memcpy(&number, &integral, 8);
	} else {
		float narrow{};
		const auto bits32 = static_cast<std::uint32_t>(integral);
		std::memcpy(&narrow, &bits32, 4);
		number = narrow;
	}
	const double limit = bits == 32 ? 2147483648.0 : 9223372036854775808.0;
	if (!(number >= -limit && number < limit))
		return indefinite;
	const auto result = static_cast<std::int64_t>(number);
	return bits == 32 ? static_cast<std::uint32_t>(result) :
		static_cast<std::uint64_t>(result);
}

/* ---- operand stream (X-AFP's oracle generator) ---- */

class Operands final {
public:
	[[nodiscard]] std::uint64_t next() noexcept
	{
		state_ ^= state_ << 13U;
		state_ ^= state_ >> 7U;
		state_ ^= state_ << 17U;
		return state_;
	}

	[[nodiscard]] std::uint64_t pick(bool dbl) noexcept
	{
		static constexpr std::array<std::uint32_t, 28> singles{
			0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
			0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x3f800000,
			0xbf800000, 0x3fc00000, 0x7f7fffff, 0xff7fffff, 0x7f800000,
			0xff800000, 0x7fc00000, 0x7fc12345, 0xffc00000, 0xffc54321,
			0x7f800001, 0x7fa00000, 0xff800001, 0xffbfffff, 0x1f800000,
			0x20000000, 0x5f800000};
		static constexpr std::array<std::uint64_t, 28> doubles{
			0, 0x8000000000000000, 1, 0x8000000000000001,
			0x000fffffffffffff, 0x800fffffffffffff, 0x0008000000000000,
			0x0010000000000000, 0x8010000000000000, 0x0010000000000001,
			0x3ff0000000000000, 0xbff0000000000000, 0x3ff8000000000000,
			0x7fefffffffffffff, 0xffefffffffffffff, 0x7ff0000000000000,
			0xfff0000000000000, 0x7ff8000000000000, 0x7ff8000000012345,
			0xfff8000000000000, 0xfff8000000054321, 0x7ff0000000000001,
			0x7ff4000000000000, 0xfff0000000000001, 0xfff7ffffffffffff,
			0x1ff0000000000000, 0x2000000000000000, 0x5ff0000000000000};
		const auto r = next();
		switch (r & 7U) {
		case 0:
		case 1:
			return dbl ? doubles[(r >> 8U) % doubles.size()] :
				singles[(r >> 8U) % singles.size()];
		case 2: {
			/* An exponent near the bottom or the top. */
			const auto e = (r >> 8U) & 7U;
			const auto m = next();
			if (dbl) {
				static constexpr std::array<std::uint64_t, 8> exponents{
					0, 1, 2, 3, 0x3fe, 0x7fd, 0x7fe, 0x7ff};
				return ((m & 1U) << 63U) | (exponents[e] << 52U) |
					(m >> 12U);
			}
			static constexpr std::array<std::uint64_t, 8> exponents{
				0, 1, 2, 3, 0x7e, 0xfd, 0xfe, 0xff};
			return ((m & 1U) << 31U) | (exponents[e] << 23U) |
				((m >> 9U) & 0x7fffffU);
		}
		case 3: {
			/* Around the square root of the smallest normal, so that
			 * products and quotients land near the threshold. */
			const auto m = next();
			if (dbl) {
				const std::uint64_t e = 0x1ff + (r >> 8U) % 5U - 2U;
				return ((m & 1U) << 63U) | (e << 52U) | (m >> 12U);
			}
			const std::uint64_t e = 0x3f + (r >> 8U) % 5U - 2U;
			return ((m & 1U) << 31U) | (e << 23U) | ((m >> 9U) & 0x7fffffU);
		}
		default:
			return dbl ? next() : (next() & 0xffffffffU);
		}
	}

	/* A first and second source whose product or quotient lies within a
	 * few units in the last place of the smallest normal, where rounding
	 * decides whether the result is tiny. */
	[[nodiscard]] std::array<std::uint64_t, 2> threshold_pair(bool dbl,
		Op op) noexcept
	{
		const Format f{dbl};
		const auto r = next();
		const auto m = next();
		/* A normal value in [2^-16, 1) for a product's first source, or in
		 * [1, 2^16) for a quotient's divisor. */
		const std::uint64_t shift = (r & 15U) + (op == Op::mul ? 1U : 0U);
		const std::uint64_t value = dbl ?
			((0x3ffULL - (op == Op::mul ? shift : 0U) +
				(op == Op::mul ? 0U : shift)) << 52U) | (m >> 12U) :
			((0x7fULL - (op == Op::mul ? shift : 0U) +
				(op == Op::mul ? 0U : shift)) << 23U) | ((m >> 9U) & 0x7fffffU);
		std::uint64_t first = value;
		std::uint64_t second = value;
		if (op == Op::mul)
			second = host(dbl, Op::div, f.minimum_normal(), value);
		else
			first = host(dbl, Op::mul, f.minimum_normal(), value);
		auto &moved = op == Op::mul ? second : first;
		moved = moved + ((r >> 8U) % 7U) - 3U;
		if ((r & 0x10000U) != 0)
			first |= f.sign();
		if ((r & 0x20000U) != 0)
			second |= f.sign();
		return {first, second};
	}

private:
	std::uint64_t state_{0x9e3779b97f4a7c15ULL};
};

/* How x86 orders a and b once DAZ has applied. */
struct Relation final {
	bool unordered{};
	bool equal{};
	bool less{};
};

[[nodiscard]] Relation compare_values(Format f, std::uint64_t a,
	std::uint64_t b, bool daz)
{
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	if (f.nan(a) || f.nan(b))
		return {true, false, false};
	if (f.dbl) {
		double x{};
		double y{};
		std::memcpy(&x, &a, 8);
		std::memcpy(&y, &b, 8);
		return {false, x == y, x < y};
	}
	const auto narrow_a = static_cast<std::uint32_t>(a);
	const auto narrow_b = static_cast<std::uint32_t>(b);
	float x{};
	float y{};
	std::memcpy(&x, &narrow_a, 4);
	std::memcpy(&y, &narrow_b, 4);
	return {false, x == y, x < y};
}

/* CMP's predicate, whose low four bits name the relation; bit 4 changes
 * only which raise invalid operation for a quiet NaN. */
[[nodiscard]] bool predicate_holds(unsigned predicate, Relation r) noexcept
{
	const bool greater = !r.unordered && !r.equal && !r.less;
	switch (predicate & 15U) {
	case 0: return r.equal;
	case 1: return r.less;
	case 2: return r.less || r.equal;
	case 3: return r.unordered;
	case 4: return !r.equal;
	case 5: return !r.less;
	case 6: return !(r.less || r.equal);
	case 7: return !r.unordered;
	case 8: return r.equal || r.unordered;
	case 9: return r.less || r.unordered;
	case 10: return !greater;
	case 11: return false;
	case 12: return !r.equal && !r.unordered;
	case 13: return greater || r.equal;
	case 14: return greater;
	default: return true;
	}
}

/* ---- instructions ---- */

enum class Shape : std::uint8_t {
	sse_scalar,  /* xmm0 = xmm0 op xmm1, lane 0; the rest of ymm0 kept */
	sse_packed,  /* xmm0 = xmm0 op xmm1; ymm0's upper half kept */
	vex_scalar,  /* lane 0 computed, the rest of xmm0 from a source, the
	              * upper half zeroed */
	vex128,      /* xmm0 computed, the upper half zeroed */
	vex256,      /* ymm0 computed */
};

/* FMA's signs: a * b + c, a * b - c, -(a * b) + c, -(a * b) - c, and the
 * two alternations, whose even lanes subtract (maddsub) or add (msubadd). */
enum class Fused : std::uint8_t { madd, msub, nmadd, nmsub, maddsub, msubadd };

enum class Conversion : std::uint8_t {
	ss_sd, sd_ss, ps_pd, pd_ps, ps_dq, pd_dq, ss_si, sd_si,
};

struct Instruction final {
	std::string name;
	Op op;
	bool dbl;
	Shape shape;
	std::vector<std::uint8_t> bytes;
	/* FMA: the form (132, 213 or 231) and the signs. */
	unsigned form{};
	Fused fused{};
	/* Conversions: which, whether it truncates, and a GPR result's
	 * width. `dbl` names a double input. */
	Conversion conversion{};
	bool truncate{};
	unsigned bits{};
	/* DPPS and DPPD: the immediate; CMP: the predicate; COMIS: 1, and
	 * UCOMIS 0. */
	std::uint8_t control{};
};

void add_arithmetic(std::vector<Instruction> &list)
{
	struct Family {
		std::string_view name;
		Op op;
		std::uint8_t opcode;
		bool single_only;
	};
	static constexpr std::array<Family, 9> families{{
		{"add", Op::add, 0x58, false},
		{"sub", Op::sub, 0x5c, false},
		{"mul", Op::mul, 0x59, false},
		{"div", Op::div, 0x5e, false},
		{"sqrt", Op::sqrt, 0x51, false},
		{"min", Op::min, 0x5d, false},
		{"max", Op::max, 0x5f, false},
		{"rcp", Op::rcp, 0x53, true},
		{"rsqrt", Op::rsqrt, 0x52, true},
	}};
	for (const auto &family : families) {
		const bool unary = family.op == Op::sqrt || family.op == Op::rcp ||
			family.op == Op::rsqrt;
		for (const bool dbl : {false, true}) {
			if (dbl && family.single_only)
				continue;
			const std::string name = std::string{family.name} +
				(dbl ? "d" : "s");
			const std::uint8_t scalar_prefix = dbl ? 0xf2 : 0xf3;
			std::vector<std::uint8_t> scalar{scalar_prefix, 0x0f,
				family.opcode, 0xc1};
			std::vector<std::uint8_t> packed;
			if (dbl)
				packed.push_back(0x66);
			packed.insert(packed.end(), {0x0f, family.opcode, 0xc1});
			/* VEX.256: vvvv names ymm1, or none for a unary operation;
			 * pp is 00 for singles and 01 (66) for doubles. */
			const std::uint8_t vvvv = unary ? 0x0f : 0x0e;
			const std::uint8_t vex = static_cast<std::uint8_t>(0x80U |
				(vvvv << 3U) | 0x04U | (dbl ? 1U : 0U));
			std::vector<std::uint8_t> wide{0xc5, vex, family.opcode, 0xc2};
			list.push_back({name, family.op, dbl, Shape::sse_scalar, scalar});
			list.push_back({name, family.op, dbl, Shape::sse_packed, packed});
			list.push_back({name, family.op, dbl, Shape::vex256, wide});
		}
	}
}

/* VFM*{132,213,231}{PS,PD,SS,SD} ymm0/xmm0, ymm1/xmm1, ymm2/xmm2: VEX.66.0F38
 * with W naming doubles and vvvv ymm1. */
void add_fused(std::vector<Instruction> &list)
{
	struct Kind {
		std::string_view name;
		Fused fused;
		std::uint8_t packed;  /* low nibble of the opcode */
		bool has_scalar;
	};
	static constexpr std::array<Kind, 6> kinds{{
		{"fmaddsub", Fused::maddsub, 0x6, false},
		{"fmsubadd", Fused::msubadd, 0x7, false},
		{"fmadd", Fused::madd, 0x8, true},
		{"fmsub", Fused::msub, 0xa, true},
		{"fnmadd", Fused::nmadd, 0xc, true},
		{"fnmsub", Fused::nmsub, 0xe, true},
	}};
	static constexpr std::array<std::pair<unsigned, std::uint8_t>, 3> forms{{
		{132, 0x90}, {213, 0xa0}, {231, 0xb0},
	}};
	for (const auto &kind : kinds)
	for (const auto &[form, base] : forms)
	for (const bool dbl : {false, true}) {
		const std::string name = std::string{kind.name} +
			std::to_string(form) + (dbl ? "d" : "s");
		const auto vex = [&](bool wide) {
			return static_cast<std::uint8_t>((dbl ? 0x80U : 0U) |
				(0x0eU << 3U) | (wide ? 0x04U : 0U) | 0x01U);
		};
		const auto opcode = static_cast<std::uint8_t>(base | kind.packed);
		list.push_back({name, Op::fma, dbl, Shape::vex128,
			{0xc4, 0xe2, vex(false), opcode, 0xc2}, form, kind.fused});
		list.push_back({name, Op::fma, dbl, Shape::vex256,
			{0xc4, 0xe2, vex(true), opcode, 0xc2}, form, kind.fused});
		if (kind.has_scalar)
			list.push_back({name, Op::fma, dbl, Shape::vex_scalar,
				{0xc4, 0xe2, vex(false),
					static_cast<std::uint8_t>(opcode + 1U), 0xc2},
				form, kind.fused});
	}
}

/* Conversions from xmm1/ymm1 to xmm0/ymm0, or to EAX or RAX. */
void add_conversions(std::vector<Instruction> &list)
{
	const auto add = [&](std::string name, bool dbl, Shape shape,
		std::vector<std::uint8_t> bytes, Conversion conversion,
		bool truncate = false, unsigned bits = 0) {
		Instruction instruction{std::move(name), Op::convert, dbl, shape,
			std::move(bytes)};
		instruction.conversion = conversion;
		instruction.truncate = truncate;
		instruction.bits = bits;
		list.push_back(std::move(instruction));
	};
	add("cvtss2sd", false, Shape::sse_scalar, {0xf3, 0x0f, 0x5a, 0xc1},
		Conversion::ss_sd);
	add("cvtsd2ss", true, Shape::sse_scalar, {0xf2, 0x0f, 0x5a, 0xc1},
		Conversion::sd_ss);
	add("cvtps2pd", false, Shape::sse_packed, {0x0f, 0x5a, 0xc1},
		Conversion::ps_pd);
	add("cvtps2pd", false, Shape::vex256, {0xc5, 0xfc, 0x5a, 0xc1},
		Conversion::ps_pd);
	add("cvtpd2ps", true, Shape::sse_packed, {0x66, 0x0f, 0x5a, 0xc1},
		Conversion::pd_ps);
	add("cvtpd2ps", true, Shape::vex256, {0xc5, 0xfd, 0x5a, 0xc1},
		Conversion::pd_ps);
	add("cvtps2dq", false, Shape::sse_packed, {0x66, 0x0f, 0x5b, 0xc1},
		Conversion::ps_dq);
	add("cvtps2dq", false, Shape::vex256, {0xc5, 0xfd, 0x5b, 0xc1},
		Conversion::ps_dq);
	add("cvttps2dq", false, Shape::sse_packed, {0xf3, 0x0f, 0x5b, 0xc1},
		Conversion::ps_dq, true);
	add("cvttps2dq", false, Shape::vex256, {0xc5, 0xfe, 0x5b, 0xc1},
		Conversion::ps_dq, true);
	add("cvtpd2dq", true, Shape::sse_packed, {0xf2, 0x0f, 0xe6, 0xc1},
		Conversion::pd_dq);
	add("cvtpd2dq", true, Shape::vex256, {0xc5, 0xff, 0xe6, 0xc1},
		Conversion::pd_dq);
	add("cvttpd2dq", true, Shape::sse_packed, {0x66, 0x0f, 0xe6, 0xc1},
		Conversion::pd_dq, true);
	add("cvttpd2dq", true, Shape::vex256, {0xc5, 0xfd, 0xe6, 0xc1},
		Conversion::pd_dq, true);
	for (const bool dbl : {false, true})
	for (const bool truncate : {false, true})
	for (const unsigned bits : {32U, 64U}) {
		std::vector<std::uint8_t> bytes{
			static_cast<std::uint8_t>(dbl ? 0xf2 : 0xf3)};
		if (bits == 64)
			bytes.push_back(0x48);
		bytes.insert(bytes.end(), {0x0f,
			static_cast<std::uint8_t>(truncate ? 0x2c : 0x2d), 0xc1});
		add(std::string{truncate ? "cvtt" : "cvt"} + (dbl ? "sd" : "ss") +
			"2si" + std::to_string(bits), dbl, Shape::sse_scalar,
			std::move(bytes), dbl ? Conversion::sd_si : Conversion::ss_si,
			truncate, bits);
	}
}

/* HADD, HSUB and ADDSUB: SSE3 xmm0 = xmm0 op xmm1, and VEX.256 ymm0 = ymm1
 * op ymm2. */
void add_horizontal(std::vector<Instruction> &list)
{
	struct Family {
		std::string_view name;
		Op op;
		std::uint8_t opcode;
	};
	static constexpr std::array<Family, 3> families{{
		{"hadd", Op::hadd, 0x7c}, {"hsub", Op::hsub, 0x7d},
		{"addsub", Op::addsub, 0xd0},
	}};
	for (const auto &family : families)
	for (const bool dbl : {false, true}) {
		const std::string name = std::string{family.name} +
			(dbl ? "pd" : "ps");
		list.push_back({name, family.op, dbl, Shape::sse_packed,
			{static_cast<std::uint8_t>(dbl ? 0x66 : 0xf2), 0x0f,
				family.opcode, 0xc1}});
		list.push_back({name, family.op, dbl, Shape::vex256,
			{0xc5, static_cast<std::uint8_t>(dbl ? 0xf5 : 0xf7),
				family.opcode, 0xc2}});
	}
}

/* DPPS and DPPD with a few immediates: SSE4.1 xmm0 = dp(xmm0, xmm1), and
 * VEX ymm0/xmm0 = dp(ymm1, ymm2). */
void add_dot_products(std::vector<Instruction> &list)
{
	for (const std::uint8_t control : {0xff, 0x7d, 0xa3, 0x3f}) {
		Instruction legacy{"dpps", Op::dot, false, Shape::sse_packed,
			{0x66, 0x0f, 0x3a, 0x40, 0xc1, control}};
		legacy.control = control;
		list.push_back(legacy);
		Instruction wide{"dpps", Op::dot, false, Shape::vex256,
			{0xc4, 0xe3, 0x75, 0x40, 0xc2, control}};
		wide.control = control;
		list.push_back(wide);
	}
	for (const std::uint8_t control : {0x33, 0x31, 0x12, 0x23}) {
		Instruction legacy{"dppd", Op::dot, true, Shape::sse_packed,
			{0x66, 0x0f, 0x3a, 0x41, 0xc1, control}};
		legacy.control = control;
		list.push_back(legacy);
		Instruction narrow{"dppd", Op::dot, true, Shape::vex128,
			{0xc4, 0xe3, 0x71, 0x41, 0xc2, control}};
		narrow.control = control;
		list.push_back(narrow);
	}
}

/* CMPSS to CMPPD with every SSE predicate, VCMPPS and VCMPPD with a few
 * of AVX's, and COMIS and UCOMIS, which compare xmm0 with xmm1. */
void add_compares(std::vector<Instruction> &list)
{
	for (const bool dbl : {false, true}) {
		for (std::uint8_t predicate = 0; predicate < 8; ++predicate) {
			const std::string name = std::string{"cmp"} +
				(dbl ? "d" : "s") + std::to_string(predicate);
			Instruction scalar{name, Op::cmp, dbl, Shape::sse_scalar,
				{static_cast<std::uint8_t>(dbl ? 0xf2 : 0xf3), 0x0f, 0xc2,
					0xc1, predicate}};
			scalar.control = predicate;
			list.push_back(scalar);
			Instruction packed{name, Op::cmp, dbl, Shape::sse_packed, {}};
			if (dbl)
				packed.bytes.push_back(0x66);
			packed.bytes.insert(packed.bytes.end(),
				{0x0f, 0xc2, 0xc1, predicate});
			packed.control = predicate;
			list.push_back(packed);
		}
		for (const std::uint8_t predicate : {0x09, 0x0d, 0x12, 0x1c}) {
			const auto vex = static_cast<std::uint8_t>(0x80U |
				(0x0eU << 3U) | 0x04U | (dbl ? 1U : 0U));
			Instruction wide{std::string{"cmp"} + (dbl ? "d" : "s") +
				std::to_string(predicate), Op::cmp, dbl, Shape::vex256,
				{0xc5, vex, 0xc2, 0xc2, predicate}};
			wide.control = predicate;
			list.push_back(wide);
		}
		for (const std::uint8_t opcode : {0x2e, 0x2f}) {
			const std::string name = std::string{opcode == 0x2f ?
				"comis" : "ucomis"} + (dbl ? "d" : "s");
			Instruction legacy{name, Op::comis, dbl, Shape::sse_scalar, {}};
			if (dbl)
				legacy.bytes.push_back(0x66);
			legacy.bytes.insert(legacy.bytes.end(), {0x0f, opcode, 0xc1});
			legacy.control = opcode == 0x2f ? 1U : 0U;
			list.push_back(legacy);
			Instruction vex{name, Op::comis, dbl, Shape::vex_scalar,
				{0xc5, static_cast<std::uint8_t>(dbl ? 0xf9 : 0xf8), opcode,
					0xc1}};
			vex.control = legacy.control;
			list.push_back(vex);
		}
	}
}

/* Roundings, conversions from integers, the second source's in memory,
 * and half-precision conversions, for the reference pass. */
void add_references(std::vector<Instruction> &list)
{
	struct Reference {
		std::string_view name;
		bool dbl;
		Shape shape;
		std::vector<std::uint8_t> bytes;
	};
	const std::vector<Reference> references{
		{"roundss/0", false, Shape::sse_scalar,
			{0x66, 0x0f, 0x3a, 0x0a, 0xc1, 0x00}},
		{"roundsd/4", true, Shape::sse_scalar,
			{0x66, 0x0f, 0x3a, 0x0b, 0xc1, 0x04}},
		{"roundps/1", false, Shape::sse_packed,
			{0x66, 0x0f, 0x3a, 0x08, 0xc1, 0x01}},
		{"roundpd/b", true, Shape::sse_packed,
			{0x66, 0x0f, 0x3a, 0x09, 0xc1, 0x0b}},
		{"vroundss/c", false, Shape::vex_scalar,
			{0xc4, 0xe3, 0x71, 0x0a, 0xc2, 0x0c}},
		{"vroundps/2", false, Shape::vex256,
			{0xc4, 0xe3, 0x7d, 0x08, 0xc1, 0x02}},
		{"vroundpd/4", true, Shape::vex256,
			{0xc4, 0xe3, 0x7d, 0x09, 0xc1, 0x04}},
		{"cvtdq2ps", false, Shape::sse_packed, {0x0f, 0x5b, 0xc1}},
		{"vcvtdq2ps", false, Shape::vex256, {0xc5, 0xfc, 0x5b, 0xc1}},
		{"cvtsi2ss/m32", false, Shape::sse_scalar,
			{0xf3, 0x0f, 0x2a, 0x46, 0x20}},
		{"cvtsi2ss/m64", false, Shape::sse_scalar,
			{0xf3, 0x48, 0x0f, 0x2a, 0x46, 0x20}},
		{"cvtsi2sd/m64", true, Shape::sse_scalar,
			{0xf2, 0x48, 0x0f, 0x2a, 0x46, 0x20}},
		{"vcvtsi2ss/m64", false, Shape::vex_scalar,
			{0xc4, 0xe1, 0xf2, 0x2a, 0x46, 0x20}},
		{"vcvtsi2sd/m64", true, Shape::vex_scalar,
			{0xc4, 0xe1, 0xf3, 0x2a, 0x46, 0x20}},
		{"vcvtph2ps", false, Shape::vex128, {0xc4, 0xe2, 0x79, 0x13, 0xc1}},
		{"vcvtph2ps", false, Shape::vex256, {0xc4, 0xe2, 0x7d, 0x13, 0xc1}},
		{"vcvtps2ph/0", false, Shape::vex128,
			{0xc4, 0xe3, 0x79, 0x1d, 0xc8, 0x00}},
		{"vcvtps2ph/4", false, Shape::vex256,
			{0xc4, 0xe3, 0x7d, 0x1d, 0xc8, 0x04}},
		{"vcvtps2ph/3", false, Shape::vex256,
			{0xc4, 0xe3, 0x7d, 0x1d, 0xc8, 0x03}},
	};
	for (const auto &reference : references)
		list.push_back({std::string{reference.name}, Op::reference,
			reference.dbl, reference.shape, reference.bytes});
}

[[nodiscard]] std::vector<Instruction> instructions()
{
	std::vector<Instruction> list;
	add_arithmetic(list);
	add_fused(list);
	add_conversions(list);
	add_horizontal(list);
	add_dot_products(list);
	add_compares(list);
	add_references(list);
	return list;
}

[[nodiscard]] const char *shape_name(Shape shape) noexcept
{
	switch (shape) {
	case Shape::sse_scalar: return "scalar";
	case Shape::sse_packed: return "packed";
	case Shape::vex_scalar: return "vexscalar";
	case Shape::vex128: return "vex128";
	case Shape::vex256: return "vex256";
	}
	return "?";
}

[[nodiscard]] std::size_t lane_count(const Instruction &instruction) noexcept
{
	return instruction.dbl ? 4U : 8U;
}

/* Lanes the instruction computes. */
[[nodiscard]] std::size_t computed_lanes(
	const Instruction &instruction) noexcept
{
	switch (instruction.shape) {
	case Shape::sse_scalar:
	case Shape::vex_scalar:
		return 1;
	case Shape::sse_packed:
	case Shape::vex128:
		return lane_count(instruction) / 2U;
	case Shape::vex256:
		return lane_count(instruction);
	}
	return 0;
}

struct Lanes final {
	std::array<std::uint64_t, 8> value{};
	std::size_t count{};
};

[[nodiscard]] std::uint64_t lane(const std::byte *bytes, bool dbl,
	std::size_t index) noexcept
{
	if (dbl) {
		std::uint64_t value{};
		std::memcpy(&value, bytes + index * 8U, 8U);
		return value;
	}
	std::uint32_t value{};
	std::memcpy(&value, bytes + index * 4U, 4U);
	return value;
}

void set_lane(std::byte *bytes, bool dbl, std::size_t index,
	std::uint64_t value) noexcept
{
	if (dbl) {
		std::memcpy(bytes + index * 8U, &value, 8U);
		return;
	}
	const auto narrow = static_cast<std::uint32_t>(value);
	std::memcpy(bytes + index * 4U, &narrow, 4U);
}

/* x86's a, b and c of a * b + c for an FMA form, as indices of ymm0 (the
 * destination), ymm1 (vvvv) and ymm2 (r/m): 132 is destination * r/m + vvvv,
 * 213 vvvv * destination + r/m, and 231 vvvv * r/m + destination. */
[[nodiscard]] std::array<std::size_t, 3> fused_operands(unsigned form) noexcept
{
	switch (form) {
	case 132: return {0, 2, 1};
	case 213: return {1, 0, 2};
	default: return {1, 2, 0};
	}
}

/* The model's ymm0 and RAX for a conversion. */
void model_conversion(const Instruction &instruction, const std::byte *input,
	bool daz, bool ftz, std::byte *output, std::uint64_t &rax)
{
	const bool vex = instruction.shape == Shape::vex256;
	if (vex)
		std::memset(output, 0, 32);
	else
		std::memcpy(output, input, 32);
	const auto *source = input + 32;
	const auto single = [&](std::size_t index) {
		return static_cast<std::uint32_t>(lane(source, false, index));
	};
	const auto dbl = [&](std::size_t index) {
		return lane(source, true, index);
	};
	switch (instruction.conversion) {
	case Conversion::ss_sd:
		set_lane(output, true, 0, x86_widen(single(0), daz));
		break;
	case Conversion::sd_ss:
		set_lane(output, false, 0, x86_narrow(dbl(0), daz, ftz));
		break;
	case Conversion::ps_pd:
		for (std::size_t index = 0; index < (vex ? 4U : 2U); ++index)
			set_lane(output, true, index, x86_widen(single(index), daz));
		break;
	case Conversion::pd_ps:
		for (std::size_t index = 0; index < 4U; ++index)
			set_lane(output, false, index, index < (vex ? 4U : 2U) ?
				x86_narrow(dbl(index), daz, ftz) : 0U);
		break;
	case Conversion::ps_dq:
		for (std::size_t index = 0; index < (vex ? 8U : 4U); ++index)
			set_lane(output, false, index, x86_to_integer(false,
				single(index), 32, instruction.truncate, daz));
		break;
	case Conversion::pd_dq:
		for (std::size_t index = 0; index < 4U; ++index)
			set_lane(output, false, index, index < (vex ? 4U : 2U) ?
				x86_to_integer(true, dbl(index), 32, instruction.truncate,
					daz) : 0U);
		break;
	case Conversion::ss_si:
	case Conversion::sd_si:
		rax = x86_to_integer(instruction.dbl, instruction.dbl ? dbl(0) :
			single(0), instruction.bits, instruction.truncate, daz);
		break;
	}
}

/* x86's DPPS or DPPD, step by step on each 128-bit half: the selected
 * products, zero elsewhere, then their sums with x86's NaN, DAZ and FTZ
 * rules at every step. Each result lane sums in its own order, which picks
 * among NaNs as x86 hardware does: for DPPS lane i adds p[i ^ 1] + p[i] and
 * then that sum + the other pair's, for DPPD p[i] + p[i ^ 1]. */
void model_dot(const Instruction &instruction, const std::byte *input,
	bool daz, bool ftz, std::byte *output)
{
	const Format f{instruction.dbl};
	const bool vex = instruction.shape != Shape::sse_packed;
	const auto *first = vex ? input + 32 : input;
	const auto *second = vex ? input + 64 : input + 32;
	const std::size_t per_half = instruction.dbl ? 2U : 4U;
	const std::size_t halves = instruction.shape == Shape::vex256 ? 2U : 1U;
	if (vex)
		std::memset(output, 0, 32);
	else
		std::memcpy(output, input, 32);
	for (std::size_t half = 0; half < halves; ++half) {
		std::array<std::uint64_t, 4> products{};
		for (std::size_t index = 0; index < per_half; ++index) {
			const auto lane_index = half * per_half + index;
			if (((instruction.control >> (4U + index)) & 1U) != 0)
				products[index] = x86_lane(f, Op::mul,
					lane(first, f.dbl, lane_index),
					lane(second, f.dbl, lane_index), daz, ftz);
		}
		std::array<std::uint64_t, 4> results{};
		if (instruction.dbl) {
			for (std::size_t index = 0; index < 2; ++index)
				results[index] = x86_lane(f, Op::add, products[index],
					products[index ^ 1U], daz, ftz);
		} else {
			std::array<std::uint64_t, 4> pairs{};
			for (std::size_t index = 0; index < 4; ++index)
				pairs[index] = x86_lane(f, Op::add, products[index ^ 1U],
					products[index], daz, ftz);
			for (std::size_t index = 0; index < 4; ++index)
				results[index] = x86_lane(f, Op::add, pairs[index],
					pairs[index ^ 2U], daz, ftz);
		}
		for (std::size_t index = 0; index < per_half; ++index)
			set_lane(output, f.dbl, half * per_half + index,
				((instruction.control >> index) & 1U) != 0 ?
					results[index] : 0U);
	}
}

/* The model's ymm0 for one iteration. */
void model_result(const Instruction &instruction, const std::byte *input,
	bool daz, bool ftz, std::byte *output)
{
	if (instruction.op == Op::convert) {
		std::uint64_t rax{};
		model_conversion(instruction, input, daz, ftz, output, rax);
		return;
	}
	const Format f{instruction.dbl};
	const std::size_t lanes = lane_count(instruction);
	const std::size_t computed = computed_lanes(instruction);
	const bool vex = instruction.shape == Shape::vex_scalar ||
		instruction.shape == Shape::vex128 ||
		instruction.shape == Shape::vex256;
	/* Lanes the instruction does not compute: an SSE form keeps ymm0's, a
	 * VEX form zeroes the upper half and a VEX scalar one takes lanes 1 and
	 * up of the low half from its first source. */
	if (instruction.op == Op::comis) {
		std::memcpy(output, input, 32);
		return;
	}
	if (vex)
		std::memset(output, 0, 32);
	else
		std::memcpy(output, input, 32);
	if (instruction.shape == Shape::vex_scalar) {
		const auto *source = instruction.op == Op::fma ? input : input + 32;
		std::memcpy(output, source, 16);
	}
	if (instruction.op == Op::dot) {
		model_dot(instruction, input, daz, ftz, output);
		return;
	}
	for (std::size_t index = 0; index < computed; ++index) {
		std::uint64_t value{};
		if (instruction.op == Op::fma) {
			const auto roles = fused_operands(instruction.form);
			const auto operand = [&](std::size_t role) {
				return lane(input + roles[role] * 32U, f.dbl, index);
			};
			bool negate_product = instruction.fused == Fused::nmadd ||
				instruction.fused == Fused::nmsub;
			bool negate_addend = instruction.fused == Fused::msub ||
				instruction.fused == Fused::nmsub;
			if (instruction.fused == Fused::maddsub)
				negate_addend = (index & 1U) == 0;
			else if (instruction.fused == Fused::msubadd)
				negate_addend = (index & 1U) != 0;
			value = x86_fused_lane(f, operand(0), operand(1), operand(2),
				negate_product, negate_addend, daz, ftz);
		} else if (instruction.op == Op::hadd ||
			instruction.op == Op::hsub || instruction.op == Op::addsub) {
			const auto *first = vex ? input + 32 : input;
			const auto *second = vex ? input + 64 : input + 32;
			/* Within each 128-bit half: HADD and HSUB combine
			 * adjacent pairs of the first source, then of the second,
			 * the lower element first; ADDSUB subtracts in even lanes. */
			const std::size_t half = lanes / 2U;
			const std::size_t base = index / half * half;
			const std::size_t position = index % half;
			std::uint64_t a{};
			std::uint64_t b{};
			Op op = Op::add;
			if (instruction.op == Op::addsub) {
				a = lane(first, f.dbl, index);
				b = lane(second, f.dbl, index);
				op = (index & 1U) == 0 ? Op::sub : Op::add;
			} else {
				const auto *source = position < half / 2U ? first : second;
				const std::size_t pair = base + 2U * (position % (half / 2U));
				a = lane(source, f.dbl, pair);
				b = lane(source, f.dbl, pair + 1U);
				op = instruction.op == Op::hadd ? Op::add : Op::sub;
			}
			value = x86_lane(f, op, a, b, daz, ftz);
		} else if (instruction.op == Op::cmp) {
			const auto *first = vex ? input + 32 : input;
			const auto *second = vex ? input + 64 : input + 32;
			value = predicate_holds(instruction.control, compare_values(f,
				lane(first, f.dbl, index), lane(second, f.dbl, index),
				daz)) ? (f.sign() | f.magnitude()) : 0U;
		} else {
			const auto *first = vex ? input + 32 : input;
			const auto *second = vex ? input + 64 : input + 32;
			value = x86_lane(f, instruction.op, lane(first, f.dbl, index),
				lane(second, f.dbl, index), daz, ftz);
		}
		set_lane(output, instruction.dbl, index, value);
	}
	(void)lanes;
}

/* COMIS's ZF, PF and CF; it clears OF, SF and AF. */
[[nodiscard]] std::uint64_t model_rflags(const Instruction &instruction,
	const std::byte *input, bool daz)
{
	const Format f{instruction.dbl};
	const auto relation = compare_values(f, lane(input, f.dbl, 0),
		lane(input + 32, f.dbl, 0), daz);
	return relation.unordered ? 0x45U : relation.equal ? 0x40U :
		relation.less ? 0x01U : 0U;
}

/* The flags COMIS defines. */
constexpr std::uint64_t comis_flags = 0x8d5U;

/* Whether x86 raises MXCSR.DE: for a denormal source in a lane it computes,
 * unless DAZ makes it zero or that lane holds a NaN, or raises invalid
 * operation or division by zero, which take precedence. A DPPS or DPPD
 * also raises it for a denormal product or sum it adds, which FTZ leaves
 * none of. RCP, RSQRT and conversions to integers raise none. */
[[nodiscard]] bool model_denormal(const Instruction &instruction,
	const std::byte *input, bool daz, bool ftz)
{
	if (daz)
		return false;
	const Format f{instruction.dbl};
	const bool vex = instruction.shape == Shape::vex_scalar ||
		instruction.shape == Shape::vex128 ||
		instruction.shape == Shape::vex256;
	const auto *first = vex ? input + 32 : input;
	const auto *second = vex ? input + 64 : input + 32;
	const std::size_t computed = computed_lanes(instruction);
	const auto binary = [&](std::uint64_t a, std::uint64_t b) {
		return (f.denormal(a) || f.denormal(b)) && !f.nan(a) && !f.nan(b);
	};
	switch (instruction.op) {
	case Op::rcp:
	case Op::rsqrt:
		return false;
	case Op::comis:
		return binary(lane(input, f.dbl, 0), lane(input + 32, f.dbl, 0));
	case Op::sqrt:
		for (std::size_t index = 0; index < computed; ++index) {
			const auto b = lane(second, f.dbl, index);
			if (f.denormal(b) && (b & f.sign()) == 0)
				return true;
		}
		return false;
	case Op::div:
		for (std::size_t index = 0; index < computed; ++index) {
			const auto b = lane(second, f.dbl, index);
			if (binary(lane(first, f.dbl, index), b) &&
				(b & f.magnitude()) != 0)
				return true;
		}
		return false;
	case Op::hadd:
	case Op::hsub: {
		const std::size_t half = lane_count(instruction) / 2U;
		for (std::size_t index = 0; index < computed; ++index) {
			const std::size_t base = index / half * half;
			const std::size_t position = index % half;
			const auto *source = position < half / 2U ? first : second;
			const std::size_t pair = base + 2U * (position % (half / 2U));
			if (binary(lane(source, f.dbl, pair),
				lane(source, f.dbl, pair + 1U)))
				return true;
		}
		return false;
	}
	case Op::fma: {
		const auto roles = fused_operands(instruction.form);
		for (std::size_t index = 0; index < computed; ++index) {
			const auto a = lane(input + roles[0] * 32U, f.dbl, index);
			const auto b = lane(input + roles[1] * 32U, f.dbl, index);
			const auto c = lane(input + roles[2] * 32U, f.dbl, index);
			if (f.nan(a) || f.nan(b) || f.nan(c) ||
				!(f.denormal(a) || f.denormal(b) || f.denormal(c)))
				continue;
			const auto zero = [&](std::uint64_t x) {
				return (x & f.magnitude()) == 0;
			};
			const auto infinite = [&](std::uint64_t x) {
				return (x & f.magnitude()) == f.exponent();
			};
			if ((zero(a) && infinite(b)) || (infinite(a) && zero(b)))
				continue;
			bool negate_product = instruction.fused == Fused::nmadd ||
				instruction.fused == Fused::nmsub;
			bool negate_addend = instruction.fused == Fused::msub ||
				instruction.fused == Fused::nmsub;
			if (instruction.fused == Fused::maddsub)
				negate_addend = (index & 1U) == 0;
			else if (instruction.fused == Fused::msubadd)
				negate_addend = (index & 1U) != 0;
			const bool product_sign = (((a ^ b) & f.sign()) != 0) !=
				negate_product;
			const bool addend_sign = ((c & f.sign()) != 0) != negate_addend;
			if ((infinite(a) || infinite(b)) && infinite(c) &&
				product_sign != addend_sign)
				continue;
			return true;
		}
		return false;
	}
	case Op::convert: {
		const auto *source = input + 32;
		const bool wide = instruction.shape == Shape::vex256;
		switch (instruction.conversion) {
		case Conversion::ss_sd:
			return Format{false}.denormal(lane(source, false, 0));
		case Conversion::sd_ss:
			return Format{true}.denormal(lane(source, true, 0));
		case Conversion::ps_pd:
		case Conversion::pd_ps: {
			const bool dbl = instruction.conversion == Conversion::pd_ps;
			for (std::size_t index = 0; index < (wide ? 4U : 2U); ++index)
				if (Format{dbl}.denormal(lane(source, dbl, index)))
					return true;
			return false;
		}
		default:
			return false;
		}
	}
	case Op::dot: {
		const std::size_t per_half = instruction.dbl ? 2U : 4U;
		const std::size_t halves =
			instruction.shape == Shape::vex256 ? 2U : 1U;
		for (std::size_t half = 0; half < halves; ++half) {
			std::array<std::uint64_t, 4> products{};
			for (std::size_t index = 0; index < per_half; ++index) {
				if (((instruction.control >> (4U + index)) & 1U) == 0)
					continue;
				const auto a = lane(first, f.dbl, half * per_half + index);
				const auto b = lane(second, f.dbl, half * per_half + index);
				if (binary(a, b))
					return true;
				products[index] = x86_lane(f, Op::mul, a, b, false, ftz);
			}
			if (binary(products[0], products[1]))
				return true;
			if (instruction.dbl)
				continue;
			if (binary(products[2], products[3]))
				return true;
			if (binary(x86_lane(f, Op::add, products[1], products[0], false,
				ftz), x86_lane(f, Op::add, products[3], products[2], false,
				ftz)))
				return true;
		}
		return false;
	}
	default:
		for (std::size_t index = 0; index < computed; ++index)
			if (binary(lane(first, f.dbl, index), lane(second, f.dbl, index)))
				return true;
		return false;
	}
}

/* One iteration's ymm0, ymm1 and ymm2: the operand stream, with a quarter
 * of the lanes of products, quotients and FMAs at the underflow threshold. */
void fill_input(const Instruction &instruction, Operands &operands,
	std::byte *input)
{
	const Format f{instruction.dbl};
	const std::size_t lanes = lane_count(instruction);
	for (std::size_t reg = 0; reg < 3; ++reg)
		for (std::size_t index = 0; index < lanes; ++index)
			set_lane(input + reg * 32U, instruction.dbl, index,
				operands.pick(instruction.dbl));
	if (instruction.op == Op::convert) {
		/* A conversion's source is ymm1; a quarter of a narrowing's lanes
		 * lie just below the smallest normal single. */
		const bool narrowing = instruction.conversion == Conversion::sd_ss ||
			instruction.conversion == Conversion::pd_ps;
		for (std::size_t index = 0; narrowing && index < lanes; ++index) {
			const auto r = operands.next();
			if ((r & 3U) != 0)
				continue;
			const std::uint64_t mantissa = ((1ULL << 52U) - 1U) -
				((r >> 8U) & 0x7fff'ffffU);
			set_lane(input + 32, true, index, ((r & 4U) != 0 ? f.sign() : 0U) |
				(0x380ULL << 52U) | mantissa);
		}
		return;
	}
	if (instruction.op != Op::mul && instruction.op != Op::div &&
		instruction.op != Op::fma)
		return;
	for (std::size_t index = 0; index < lanes; ++index) {
		if ((operands.next() & 3U) != 0)
			continue;
		if (instruction.op == Op::fma) {
			/* a * b at the threshold, and a small addend that moves
			 * the fused result in steps of the least denormal. */
			const auto pair = operands.threshold_pair(instruction.dbl,
				Op::mul);
			const auto r = operands.next();
			const std::uint64_t addend = ((r & 7U) == 0 ? 0U : (r >> 8U) % 9U) |
				((r & 8U) != 0 ? f.sign() : 0U);
			const auto roles = fused_operands(instruction.form);
			set_lane(input + roles[0] * 32U, f.dbl, index, pair[0]);
			set_lane(input + roles[1] * 32U, f.dbl, index, pair[1]);
			set_lane(input + roles[2] * 32U, f.dbl, index, addend);
			continue;
		}
		auto *first = instruction.shape == Shape::vex256 ? input + 32 : input;
		const auto pair = operands.threshold_pair(instruction.dbl,
			instruction.op);
		set_lane(first, instruction.dbl, index, pair[0]);
		set_lane(first + 32, instruction.dbl, index, pair[1]);
	}
}

struct Totals final {
	std::uint64_t lanes{};
	std::uint64_t differing{};
	std::uint64_t kept_differing{};
	bool engine_failed{};
};

struct Shown final {
	unsigned count{};
};

/* A conversion's ymm0, compared dword by dword, and a GPR result. */
void compare_conversion(const Instruction &instruction, const std::byte *input,
	const std::byte *expected, std::uint64_t rax, const std::byte *actual,
	bool daz, bool ftz, unsigned rounding, Totals &totals, Shown &shown)
{
	const bool gpr = instruction.conversion == Conversion::ss_si ||
		instruction.conversion == Conversion::sd_si;
	for (std::size_t index = 0; index < 9U; ++index) {
		if (index == 8U && !gpr)
			break;
		std::uint64_t want{};
		std::uint64_t got{};
		if (index == 8U) {
			want = rax;
			std::memcpy(&got, actual + 32, 8);
		} else {
			want = lane(expected, false, index);
			got = lane(actual, false, index);
		}
		++totals.lanes;
		if (want == got)
			continue;
		++totals.differing;
		if (shown.count < 12U) {
			++shown.count;
			std::fprintf(stderr, "  %s %s daz=%d ftz=%d rc=%u %s %zu "
				"source=%#llx engine=%#llx x86=%#llx\n",
				instruction.name.c_str(), shape_name(instruction.shape),
				daz ? 1 : 0, ftz ? 1 : 0, rounding,
				index == 8U ? "rax" : "dword", index,
				static_cast<unsigned long long>(lane(input + 32,
					instruction.dbl, 0)),
				static_cast<unsigned long long>(got),
				static_cast<unsigned long long>(want));
		}
	}
}

Totals run_cell(const Instruction &instruction, bool daz, bool ftz,
	unsigned rounding, std::size_t iterations, Shown &shown,
	bool checked = false)
{
	FlatMemory memory{memory_size};
	const auto code = loop_code(instruction.bytes);
	(void)memory.write(code_address, code);
#if UML_SSE_EXACTNESS_JIT
	auto &executor = jit_engine();
#else
	uml::cpu::x86_64::gadgets::Executor executor;
#endif
	/* Checked: MXCSR unmasks a denormal operand alone. */
	const std::uint32_t mxcsr = (checked ? 0x1e80U : 0x1f80U) |
		(daz ? 0x40U : 0U) | (ftz ? 0x8000U : 0U) | (rounding << 13U);
	{
		std::array<std::byte, 4> control{};
		std::memcpy(control.data(), &mxcsr, 4);
		(void)memory.write(control_address, control);
	}
#if !UML_SSE_EXACTNESS_JIT
	if (checked)
		executor.check_exceptions(uml::cpu::x86_64::gadgets::check_simd);
	executor.prepare(memory);
#endif
	Operands operands;
	Totals totals;
	const std::size_t lanes = lane_count(instruction);
	const std::size_t width = computed_lanes(instruction);
	std::vector<std::byte> expected(32);
	for (std::size_t done = 0; done < iterations;) {
		const auto count = std::min(chunk, iterations - done);
		for (std::size_t row = 0; row < count; ++row)
			fill_input(instruction, operands,
				memory.at(input_address + row * input_stride));
		std::vector<std::uint8_t> trapped(count);
		if (!run_engine(memory, executor, mxcsr, count, checked,
			instruction.bytes.size(), &trapped)) {
			totals.engine_failed = true;
			return totals;
		}
		const HostRounding host_rounding{rounding};
		for (std::size_t row = 0; row < count; ++row) {
			const auto *input = memory.at(input_address + row * input_stride);
			const auto *actual = memory.at(output_address +
				row * output_stride);
			if (checked) {
				/* #XM for exactly the rows x86 raises DE for, with
				 * MXCSR.DE set and the destination kept. */
				const bool denormal = model_denormal(instruction, input,
					daz, ftz);
				std::uint32_t stored{};
				std::memcpy(&stored, actual + 48, 4);
				++totals.lanes;
				const bool kept = std::memcmp(actual, input, 32) == 0;
				const bool right = trapped[row] != 0 ?
					denormal && (stored & 0x02U) != 0 && kept :
					!denormal && (stored & 0x02U) == 0;
				if (!right) {
					++totals.differing;
					if (shown.count < 12U) {
						++shown.count;
						std::fprintf(stderr, "  %s %s daz=%d ftz=%d rc=%u "
							"checked: trapped %d x86 DE %d mxcsr %#x "
							"ymm0=%#llx ymm1=%#llx ymm2=%#llx\n",
							instruction.name.c_str(),
							shape_name(instruction.shape), daz ? 1 : 0,
							ftz ? 1 : 0, rounding, trapped[row],
							denormal ? 1 : 0, stored,
							static_cast<unsigned long long>(
								lane(input, instruction.dbl, 0)),
							static_cast<unsigned long long>(
								lane(input + 32, instruction.dbl, 0)),
							static_cast<unsigned long long>(
								lane(input + 64, instruction.dbl, 0)));
					}
				}
				if (trapped[row] != 0)
					continue;
			}
			if (instruction.op == Op::convert) {
				std::uint64_t rax{};
				model_conversion(instruction, input, daz, ftz,
					expected.data(), rax);
				compare_conversion(instruction, input, expected.data(), rax,
					actual, daz, ftz, rounding, totals, shown);
				continue;
			}
			model_result(instruction, input, daz, ftz, expected.data());
			if (instruction.op == Op::comis) {
				std::uint64_t flags{};
				std::memcpy(&flags, actual + 40, 8);
				const auto want = model_rflags(instruction, input, daz);
				++totals.lanes;
				if ((flags & comis_flags) != want) {
					++totals.differing;
					if (shown.count < 12U) {
						++shown.count;
						std::fprintf(stderr, "  %s %s daz=%d xmm0=%#llx "
							"xmm1=%#llx engine rflags=%#llx x86=%#llx\n",
							instruction.name.c_str(),
							shape_name(instruction.shape), daz ? 1 : 0,
							static_cast<unsigned long long>(
								lane(input, instruction.dbl, 0)),
							static_cast<unsigned long long>(
								lane(input + 32, instruction.dbl, 0)),
							static_cast<unsigned long long>(
								flags & comis_flags),
							static_cast<unsigned long long>(want));
					}
				}
			}
			for (std::size_t index = 0; index < lanes; ++index) {
				const auto want = lane(expected.data(), instruction.dbl, index);
				const auto got = lane(actual, instruction.dbl, index);
				const bool operated = index < width;
				if (operated)
					++totals.lanes;
				if (want == got)
					continue;
				if (operated)
					++totals.differing;
				else
					++totals.kept_differing;
				if (shown.count < 12U) {
					++shown.count;
					std::fprintf(stderr, "  %s %s daz=%d ftz=%d rc=%u lane %zu "
						"ymm0=%#llx ymm1=%#llx ymm2=%#llx engine=%#llx "
						"x86=%#llx\n", instruction.name.c_str(),
						shape_name(instruction.shape), daz ? 1 : 0,
						ftz ? 1 : 0, rounding, index,
						static_cast<unsigned long long>(
							lane(input, instruction.dbl, index)),
						static_cast<unsigned long long>(
							lane(input + 32, instruction.dbl, index)),
						static_cast<unsigned long long>(
							lane(input + 64, instruction.dbl, index)),
						static_cast<unsigned long long>(got),
						static_cast<unsigned long long>(want));
				}
			}
		}
		done += count;
	}
	return totals;
}

#if UML_SSE_EXACTNESS_JIT
/* MXCSRs that unmask exceptions: some of invalid operation, division by zero
 * and overflow, whose special checks test values in line, and others, which
 * every check settles. */
constexpr std::array<std::uint32_t, 5> reference_controls{
	0x1900U, 0x1d80U, 0x1680U, 0x1380U, 0x0000U};

/* The JIT against the gadget engine under each of `reference_controls`. An
 * instruction faults in both or in neither: with the same flags and its
 * destination kept, or with the same results. While MXCSR unmasks a
 * denormal operand, underflow or precision, every check of both engines
 * settles exceptions, and the flags match too; else only the instructions a
 * check sends to the model raise flags, which the engines choose apart. */
Totals run_reference_cell(const Instruction &instruction, bool daz,
	bool ftz, unsigned rounding, std::size_t iterations, Shown &shown)
{
	FlatMemory memory{memory_size};
	const auto code = loop_code(instruction.bytes);
	(void)memory.write(code_address, code);
	auto &jit = jit_engine();
	const std::uint32_t modes = (daz ? 0x40U : 0U) | (ftz ? 0x8000U : 0U) |
		(rounding << 13U);
	Operands operands;
	Totals totals;
	std::vector<std::byte> reference(chunk * output_stride);
	for (std::size_t done = 0; done < iterations;) {
		const auto count = std::min(chunk, iterations - done);
		for (std::size_t row = 0; row < count; ++row)
			fill_input(instruction, operands,
				memory.at(input_address + row * input_stride));
		for (const auto masks : reference_controls) {
			const auto mxcsr = masks | modes;
			std::array<std::byte, 4> control{};
			std::memcpy(control.data(), &mxcsr, 4);
			(void)memory.write(control_address, control);
			std::vector<std::uint8_t> trapped_gadgets(count);
			std::vector<std::uint8_t> trapped_jit(count);
			uml::cpu::x86_64::gadgets::Executor gadgets;
			gadgets.check_exceptions(uml::cpu::x86_64::gadgets::check_simd);
			gadgets.prepare(memory);
			if (!run_engine(memory, gadgets, mxcsr, count, true,
				instruction.bytes.size(), &trapped_gadgets)) {
				totals.engine_failed = true;
				return totals;
			}
			std::memcpy(reference.data(), memory.at(output_address),
				count * output_stride);
			if (!run_engine(memory, jit, mxcsr, count, true,
				instruction.bytes.size(), &trapped_jit)) {
				totals.engine_failed = true;
				return totals;
			}
			/* Unmasked: a denormal operand, underflow or precision. */
			const bool all_checks = (~(mxcsr >> 7U) & 0x32U) != 0;
			for (std::size_t row = 0; row < count; ++row) {
				const auto *input = memory.at(input_address +
					row * input_stride);
				const auto *actual = memory.at(output_address +
					row * output_stride);
				const auto *expected = reference.data() + row * output_stride;
				std::uint32_t flags_jit{};
				std::uint32_t flags_gadgets{};
				std::memcpy(&flags_jit, actual + 48, 4);
				std::memcpy(&flags_gadgets, expected + 48, 4);
				const bool same_flags = (flags_jit & 0x3fU) ==
					(flags_gadgets & 0x3fU);
				const bool faulted = trapped_gadgets[row] != 0;
				++totals.lanes;
				const bool right = faulted != (trapped_jit[row] != 0) ? false :
					faulted ? same_flags &&
						std::memcmp(actual, input, 32) == 0 :
					std::memcmp(actual, expected, 48) == 0 &&
						(!all_checks || same_flags);
				if (right)
					continue;
				++totals.differing;
				if (shown.count < 12U) {
					++shown.count;
					std::fprintf(stderr, "  %s %s daz=%d ftz=%d rc=%u "
						"reference mxcsr %#x: trapped %d/%d flags %#x/%#x "
						"ymm0=%#llx ymm1=%#llx ymm2=%#llx\n",
						instruction.name.c_str(),
						shape_name(instruction.shape), daz ? 1 : 0,
						ftz ? 1 : 0, rounding, mxcsr, trapped_jit[row],
						trapped_gadgets[row], flags_jit, flags_gadgets,
						static_cast<unsigned long long>(
							lane(input, instruction.dbl, 0)),
						static_cast<unsigned long long>(
							lane(input + 32, instruction.dbl, 0)),
						static_cast<unsigned long long>(
							lane(input + 64, instruction.dbl, 0)));
				}
			}
		}
		done += count;
	}
	return totals;
}
#endif

} // namespace

int main(int argc, char **argv)
{
	std::size_t iterations = 2000;
	std::size_t checked_iterations = 200;
	std::size_t reference_iterations = 100;
	std::string_view only;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument{argv[index]};
		if (argument == "--iterations" && index + 1 < argc)
			iterations = std::strtoull(argv[++index], nullptr, 10);
		else if (argument == "--checked-iterations" && index + 1 < argc)
			checked_iterations = std::strtoull(argv[++index], nullptr, 10);
		else if (argument == "--reference-iterations" && index + 1 < argc)
			reference_iterations = std::strtoull(argv[++index], nullptr, 10);
		else if (argument == "--only" && index + 1 < argc)
			only = argv[++index];
		else {
			std::fprintf(stderr, "usage: %s [--iterations N] "
				"[--checked-iterations N] [--reference-iterations N] "
				"[--only TEXT]\n", argv[0]);
			return 2;
		}
	}
	std::uint64_t lanes{};
	std::uint64_t differing{};
	std::uint64_t kept{};
	bool failed = false;
	Shown shown;
	std::printf("instruction\tshape\tdaz\tftz\trc\tlanes\tdiffering\n");
	/* Plain, checked, and for the JIT against the gadget engine. */
#if UML_SSE_EXACTNESS_JIT
	const unsigned passes = 3;
#else
	const unsigned passes = 2;
#endif
	for (unsigned pass = 0; pass < passes; ++pass)
	for (const auto &instruction : instructions()) {
		if (!only.empty() &&
			instruction.name.find(only) == std::string::npos)
			continue;
		if (pass < 2 && instruction.op == Op::reference)
			continue;
		const bool checked = pass == 1;
		for (const bool daz : {false, true})
		for (const bool ftz : {false, true})
		for (unsigned rounding = 0; rounding < 4; ++rounding) {
#if UML_SSE_EXACTNESS_JIT
			const auto totals = pass == 2 ?
				run_reference_cell(instruction, daz, ftz, rounding,
					reference_iterations, shown) :
				run_cell(instruction, daz, ftz, rounding,
					checked ? checked_iterations : iterations, shown, checked);
#else
			(void)reference_iterations;
			const auto totals = run_cell(instruction, daz, ftz, rounding,
				checked ? checked_iterations : iterations, shown, checked);
#endif
			if (totals.engine_failed) {
				std::fprintf(stderr, "x86-64 SSE exactness: %s did not run "
					"to its end\n", instruction.name.c_str());
				failed = true;
				continue;
			}
			lanes += totals.lanes;
			differing += totals.differing;
			kept += totals.kept_differing;
			std::printf("%s%s\t%s\t%d\t%d\t%u\t%llu\t%llu\n",
				instruction.name.c_str(),
				pass == 2 ? "/reference" : checked ? "/checked" : "",
				shape_name(instruction.shape), daz ? 1 : 0, ftz ? 1 : 0,
				rounding, static_cast<unsigned long long>(totals.lanes),
				static_cast<unsigned long long>(totals.differing));
		}
	}
	std::printf("TOTAL lanes %llu differing %llu kept-lane mismatches %llu\n",
		static_cast<unsigned long long>(lanes),
		static_cast<unsigned long long>(differing),
		static_cast<unsigned long long>(kept));
	return failed || differing != 0 || kept != 0 ? 1 : 0;
}
