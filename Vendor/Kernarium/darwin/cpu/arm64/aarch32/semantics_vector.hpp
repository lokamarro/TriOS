// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/semantics_internal.hpp"
#include "cpu/arm64/aarch32/state_format.hpp"

#include <array>
#include <cstdint>

/*
 * What the library's VFP and Advanced SIMD sources share: the lane helpers
 * (a D register as a 64-bit value, elements by bit width, exact integer
 * arithmetic as the pseudocode's unbounded integers), the saturation and
 * rounding-shift primitives, and the FPSCR split.
 */

namespace uml::cpu::arm64::aarch32::detail {

using i128 = __int128;
using u128 = unsigned __int128;

/* FPSCR is split across CpuState as state_format.hpp describes: the control
 * bits in fpcr (fpscr_control_mask), QC and the cumulative flags in fpsr
 * (fpsr_writable_mask), and N, Z, C and V in a32_fpscr_nzcv (fpscr_nzcv),
 * because the hosts implement FPSR[31:28] as RAZ/WI. Len, Stride and the trap
 * enables read as zero and ignore writes. fpscr_status_mask is the bridge's
 * status half, N, Z, C and V included, so it is not what fpsr holds. */
inline constexpr std::uint32_t fpscr_qc = 1U << 27;

[[nodiscard]] inline std::uint32_t fpscr_of(const CpuState &cpu) noexcept
{
	return (cpu.fpcr & fpscr_control_mask) | (cpu.fpsr & fpsr_writable_mask)
		| (cpu.a32_fpscr_nzcv & fpscr_nzcv);
}

inline void set_fpscr(CpuState &cpu, std::uint32_t value) noexcept
{
	cpu.fpcr = value & fpscr_control_mask;
	cpu.fpsr = value & fpsr_writable_mask;
	cpu.a32_fpscr_nzcv = value & fpscr_nzcv;
}

/* The low `bits` bits of a lane. */
[[nodiscard]] constexpr std::uint64_t lane_mask(unsigned bits) noexcept
{
	return bits >= 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
}

/* Element `index` of `bits` bits in a 64-bit D value. */
[[nodiscard]] constexpr std::uint64_t lane(std::uint64_t value, unsigned index,
	unsigned bits) noexcept
{
	return bits >= 64U ? value : value >> (index * bits) & lane_mask(bits);
}

[[nodiscard]] constexpr std::uint64_t with_lane(std::uint64_t value, unsigned index,
	unsigned bits, std::uint64_t element) noexcept
{
	if (bits >= 64U)
		return element;
	const unsigned shift = index * bits;
	return (value & ~(lane_mask(bits) << shift)) | (element & lane_mask(bits)) << shift;
}

/* UInt() or SInt() of a lane. */
[[nodiscard]] constexpr i128 lane_value(std::uint64_t raw, unsigned bits,
	bool is_signed) noexcept
{
	raw &= lane_mask(bits);
	if (is_signed && bits < 64U && (raw >> (bits - 1U) & 1U) != 0U)
		return static_cast<i128>(raw) - (i128{1} << bits);
	if (is_signed && bits == 64U)
		return static_cast<std::int64_t>(raw);
	return static_cast<i128>(raw);
}

/* The low `bits` bits of an integer, as a lane. */
[[nodiscard]] constexpr std::uint64_t lane_bits(i128 value, unsigned bits) noexcept
{
	return static_cast<std::uint64_t>(value) & lane_mask(bits);
}

struct Saturation final {
	std::uint64_t raw;
	bool saturated;
};

/* SatQ(): the value saturated to `bits` bits, signed or unsigned. */
[[nodiscard]] constexpr Saturation sat_q(i128 value, unsigned bits, bool unsigned_range) noexcept
{
	const i128 upper = unsigned_range ? (i128{1} << bits) - 1 : (i128{1} << (bits - 1U)) - 1;
	const i128 lower = unsigned_range ? 0 : -(i128{1} << (bits - 1U));
	if (value > upper)
		return {lane_bits(upper, bits), true};
	if (value < lower)
		return {lane_bits(lower, bits), true};
	return {lane_bits(value, bits), false};
}

/* The integer shifted left by 0 to 255 places. A shift that no 64-bit lane
 * can hold keeps only its sign, which saturates and truncates alike. */
[[nodiscard]] constexpr i128 shift_left_exact(i128 value, unsigned amount) noexcept
{
	if (value == 0)
		return 0;
	if (amount >= 64U)
		return value < 0 ? -(i128{1} << 100) : i128{1} << 100;
	return value * (i128{1} << amount);
}

/* RShr(): the integer shifted right by `amount` places, rounding to nearest
 * (up on a tie) when `round`. */
[[nodiscard]] constexpr i128 shift_right(i128 value, unsigned amount, bool round) noexcept
{
	if (amount > 100U)
		amount = 100U;
	if (amount == 0U)
		return value;
	if (round)
		value += i128{1} << (amount - 1U);
	return value >> amount;
}

/* A function of each element of up to two D registers. */
template<typename Function>
[[nodiscard]] constexpr std::uint64_t map_lanes(std::uint64_t a, std::uint64_t b,
	unsigned bits, Function &&function) noexcept
{
	std::uint64_t out = 0;
	for (unsigned offset = 0; offset < 64U; offset += bits)
		out |= (function(lane(a, offset / bits, bits), lane(b, offset / bits, bits))
				& lane_mask(bits))
			<< offset;
	return out;
}

/* A function of the n, m and old d operands of each D register of a D or Q
 * form. Every source is read before any destination is written. */
template<typename Function>
inline bool for_each_register(Exec &x, Function &&function)
{
	const Operands &o = x.o;
	const unsigned regs = static_cast<unsigned>(o.regs);
	std::array<std::uint64_t, 2> n{}, m{}, d{};
	for (unsigned r = 0; r < regs; ++r) {
		n[r] = x.d_reg(static_cast<unsigned>(o.n) + r);
		m[r] = x.d_reg(static_cast<unsigned>(o.m) + r);
		d[r] = x.d_reg(static_cast<unsigned>(o.d) + r);
	}
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, function(n[r], m[r], d[r]));
	return true;
}

/* QC: the cumulative saturation flag, in the FPSR. */
inline void set_qc(CpuState &cpu) noexcept
{
	cpu.fpsr |= fpscr_qc;
}

/* Q(d >> 1) as the pair D(d), D(d + 1) for an even d. */
[[nodiscard]] inline u128 q_reg(const Exec &x, unsigned d) noexcept
{
	return static_cast<u128>(x.d_reg(d + 1U)) << 64 | x.d_reg(d);
}
inline void set_q_reg(Exec &x, unsigned d, u128 value) noexcept
{
	x.set_d_reg(d, static_cast<std::uint64_t>(value));
	x.set_d_reg(d + 1U, static_cast<std::uint64_t>(value >> 64));
}

/* Element `index` of `bits` bits (up to 64) of a Q value. */
[[nodiscard]] constexpr std::uint64_t lane128(u128 value, unsigned index, unsigned bits) noexcept
{
	return static_cast<std::uint64_t>(value >> (index * bits)) & lane_mask(bits);
}
[[nodiscard]] constexpr u128 with_lane128(u128 value, unsigned index, unsigned bits,
	std::uint64_t element) noexcept
{
	const u128 mask = static_cast<u128>(lane_mask(bits)) << (index * bits);
	return (value & ~mask) | static_cast<u128>(element & lane_mask(bits)) << (index * bits);
}

/* PolynomialMult(): the carry-less product of two polynomials over GF(2). */
[[nodiscard]] constexpr u128 polynomial_multiply(std::uint64_t a, std::uint64_t b) noexcept
{
	u128 result = 0;
	for (unsigned bit = 0; bit < 64U; ++bit)
		if ((b >> bit & 1U) != 0U)
			result ^= static_cast<u128>(a) << bit;
	return result;
}

using Raw = std::uint64_t;

[[nodiscard]] inline unsigned element_bits(const Exec &x) noexcept
{
	return static_cast<unsigned>(x.o.esize);
}

/* The QC bookkeeping of the saturating forms: each lane's result passes
 * through take(), and finish() sets QC when any lane saturated. */
class QcTally final {
public:
	Raw take(const Saturation &result) noexcept
	{
		saturated_ = saturated_ || result.saturated;
		return result.raw;
	}
	[[nodiscard]] bool finish(Exec &x, bool completed) const noexcept
	{
		if (saturated_)
			set_qc(x.cpu);
		return completed;
	}

private:
	bool saturated_ = false;
};

[[nodiscard]] inline i128 absolute(i128 value) noexcept
{
	return value < 0 ? -value : value;
}

/* A function of the n, m and old d elements of each D register of a D or Q
 * form, all elements esize wide. */
template<typename Function>
inline bool lanewise(Exec &x, Function &&function)
{
	const unsigned bits = element_bits(x);
	const Raw mask = lane_mask(bits);
	return for_each_register(x, [&](Raw n, Raw m, Raw d) -> Raw {
		Raw out = 0;
		for (unsigned offset = 0; offset < 64U; offset += bits)
			out |= (function(n >> offset & mask, m >> offset & mask, d >> offset & mask) & mask)
				<< offset;
		return out;
	});
}

/* The same with the m operand one element of D(m): the by-scalar forms. */
template<typename Function>
inline bool lanewise_scalar(Exec &x, Function &&function)
{
	const unsigned bits = element_bits(x);
	const Raw mask = lane_mask(bits);
	const Raw scalar = lane(x.d_reg(static_cast<unsigned>(x.o.m)),
		static_cast<unsigned>(x.o.index_value), bits);
	return for_each_register(x, [&](Raw n, Raw, Raw d) -> Raw {
		Raw out = 0;
		for (unsigned offset = 0; offset < 64U; offset += bits)
			out |= (function(n >> offset & mask, scalar, d >> offset & mask) & mask) << offset;
		return out;
	});
}

/* The long forms: D operands, a Q result of twice-as-wide elements. `wide`
 * is a Q first operand (VADDW, VSUBW); `scalar` takes the second operand
 * from one element of D(m). The function receives the n element, the m
 * element and the old d element, and returns the wide result. */
template<typename Function>
inline bool long_lanes(Exec &x, bool wide, bool scalar, Function &&function)
{
	const Operands &o = x.o;
	const unsigned bits = element_bits(x);
	const unsigned elements = 64U / bits;
	const unsigned d = static_cast<unsigned>(o.d);
	const unsigned n = static_cast<unsigned>(o.n);
	const unsigned m = static_cast<unsigned>(o.m);
	const u128 old_d = q_reg(x, d);
	const u128 wide_n = wide ? q_reg(x, n) : u128{0};
	const Raw narrow_n = wide ? Raw{0} : x.d_reg(n);
	const Raw source_m = x.d_reg(m);
	u128 out = 0;
	for (unsigned e = 0; e < elements; ++e) {
		const Raw first = wide ? lane128(wide_n, e, 2U * bits) : lane(narrow_n, e, bits);
		const Raw second = scalar ? lane(source_m, static_cast<unsigned>(o.index_value), bits)
					   : lane(source_m, e, bits);
		out = with_lane128(out, e, 2U * bits, function(first, second, lane128(old_d, e, 2U * bits)));
	}
	set_q_reg(x, d, out);
	return true;
}

/* The narrow forms: Q operands, a D result of half-width elements. The
 * function receives the n and m elements (twice esize wide) and returns the
 * result element. */
template<typename Function>
inline bool narrow_lanes(Exec &x, Function &&function)
{
	const Operands &o = x.o;
	const unsigned bits = element_bits(x);
	const unsigned elements = 64U / bits;
	const u128 first = q_reg(x, static_cast<unsigned>(o.n));
	const u128 second = q_reg(x, static_cast<unsigned>(o.m));
	Raw out = 0;
	for (unsigned e = 0; e < elements; ++e)
		out = with_lane(out, e, bits,
			function(lane128(first, e, 2U * bits), lane128(second, e, 2U * bits)));
	x.set_d_reg(static_cast<unsigned>(o.d), out);
	return true;
}

} // namespace uml::cpu::arm64::aarch32::detail
