// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's Advanced SIMD shifts: by immediate and by register,
// rounding, saturating, accumulating, narrowing and widening, and the
// insert forms VSLI and VSRI.

#include "cpu/arm64/aarch32/semantics_vector.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

/* The shift of a by-register form: the signed low byte of the n element. */
[[nodiscard]] int register_shift(Raw n) noexcept
{
	return static_cast<std::int8_t>(n & 0xffU);
}

[[nodiscard]] unsigned amount(const Exec &x) noexcept
{
	return static_cast<unsigned>(x.o.shift_amount);
}

/* The element of the m operand as an integer, for the signedness the form
 * records. */
[[nodiscard]] i128 operand(const Exec &x, Raw m) noexcept
{
	return lane_value(m, element_bits(x), !x.o.unsigned_);
}

/* A positive shift moves the element left, a negative one right (rounded
 * to nearest when `round`). */
[[nodiscard]] i128 shifted(i128 element, int shift, bool round) noexcept
{
	return shift >= 0 ? shift_left_exact(element, static_cast<unsigned>(shift))
			  : shift_right(element, static_cast<unsigned>(-shift), round);
}

bool vshl_i(Exec &x)
{
	const unsigned shift = amount(x);
	return lanewise(x, [&](Raw, Raw m, Raw) { return m << shift; });
}

bool vshl_r(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		return lane_bits(shifted(operand(x, m), register_shift(n), false), bits);
	});
}

bool vrshl(Exec &x)
{
	const unsigned bits = element_bits(x);
	return lanewise(x, [&](Raw n, Raw m, Raw) {
		return lane_bits(shifted(operand(x, m), register_shift(n), true), bits);
	});
}

/* VSHR, VRSHR and the accumulating VSRA and VRSRA. */
template<bool Round, bool Accumulate>
bool vshr(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	return lanewise(x, [&](Raw, Raw m, Raw d) {
		const i128 result = shift_right(operand(x, m), shift, Round);
		return lane_bits(Accumulate ? result + d : result, bits);
	});
}

/* VSHRN and VRSHRN: a logical shift of the wide element, then the low half. */
template<bool Round>
bool vshrn(Exec &x)
{
	const unsigned shift = amount(x);
	return narrow_lanes(x, [&](Raw, Raw m) {
		return static_cast<Raw>(shift_right(static_cast<i128>(m), shift, Round));
	});
}

/* VQSHRN, VQSHRUN and the rounding forms. */
template<bool Round>
bool vqshrn(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	const bool source_unsigned = x.o.src_unsigned;
	const bool destination_unsigned = x.o.dest_unsigned;
	QcTally qc;
	const bool completed = narrow_lanes(x, [&](Raw, Raw m) {
		const i128 element = lane_value(m, 2U * bits, !source_unsigned);
		return qc.take(sat_q(shift_right(element, shift, Round), bits, destination_unsigned));
	});
	return qc.finish(x, completed);
}

/* VQSHL and VQSHLU by immediate. */
bool vqshl_i(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	const bool source_unsigned = x.o.src_unsigned;
	const bool destination_unsigned = x.o.dest_unsigned;
	QcTally qc;
	const bool completed = lanewise(x, [&](Raw, Raw m, Raw) {
		return qc.take(sat_q(
			shift_left_exact(lane_value(m, bits, !source_unsigned), shift), bits, destination_unsigned));
	});
	return qc.finish(x, completed);
}

/* VQSHL and VQRSHL by register. */
template<bool Round>
bool vqshl_r(Exec &x)
{
	const unsigned bits = element_bits(x);
	const bool is_unsigned = x.o.unsigned_;
	bool saturated = false;
	const bool completed = lanewise(x, [&](Raw n, Raw m, Raw) {
		const Saturation result =
			sat_q(shifted(operand(x, m), register_shift(n), Round), bits, is_unsigned);
		saturated = saturated || result.saturated;
		return result.raw;
	});
	if (saturated)
		set_qc(x.cpu);
	return completed;
}

bool vshll(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	return long_lanes(x, false, false, [&](Raw, Raw m, Raw) {
		return lane_bits(shift_left_exact(operand(x, m), shift), 2U * bits);
	});
}

/* VSLI and VSRI: the shifted m bits replace the d bits they cover. */
bool vsli(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	const Raw mask = lane_mask(bits) << shift & lane_mask(bits);
	return lanewise(x, [&](Raw, Raw m, Raw d) { return (d & ~mask) | (m << shift & lane_mask(bits)); });
}

bool vsri(Exec &x)
{
	const unsigned bits = element_bits(x);
	const unsigned shift = amount(x);
	const Raw mask = shift >= bits ? Raw{0} : lane_mask(bits) >> shift;
	return lanewise(x, [&](Raw, Raw m, Raw d) {
		return (d & ~mask) | (shift >= bits ? Raw{0} : m >> shift);
	});
}

constexpr VectorEntry entries[] = {
	{"VSHL_i", vshl_i}, {"VSHL_r", vshl_r}, {"VRSHL", vrshl},
	{"VSHR", vshr<false, false>}, {"VRSHR", vshr<true, false>},
	{"VSRA", vshr<false, true>}, {"VRSRA", vshr<true, true>},
	{"VSHRN", vshrn<false>}, {"VRSHRN", vshrn<true>},
	{"VQSHRN", vqshrn<false>}, {"VQSHRUN", vqshrn<false>},
	{"VQRSHRN", vqshrn<true>}, {"VQRSHRUN", vqshrn<true>},
	{"VQSHL_i", vqshl_i}, {"VQSHLU_i", vqshl_i}, {"VQSHL_r", vqshl_r<false>},
	{"VQRSHL", vqshl_r<true>}, {"VSHLL", vshll}, {"VSLI", vsli}, {"VSRI", vsri},
};

} // namespace

std::span<const VectorEntry> neon_shift_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
