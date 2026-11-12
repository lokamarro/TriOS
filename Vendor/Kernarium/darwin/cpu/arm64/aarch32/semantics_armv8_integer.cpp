// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's Armv8 integer extensions: the rounding doubling
// multiply-accumulate returning high half VQRDMLAH and VQRDMLSH, the 8-bit
// dot products (VSDOT, VUDOT, VUSDOT, VSUDOT) and 8-bit matrix multiply
// accumulates (VSMMLA, VUMMLA, VUSMMLA), and the CRC32 instructions. All of
// it is exact integer arithmetic.

#include "cpu/arm64/aarch32/semantics_vector.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using std::uint32_t;
using std::uint64_t;

/* VQRDMLAH and VQRDMLSH, by vector or by element: the accumulator shifted
 * up by the element size, plus or minus twice the product, rounded and
 * shifted back down, saturating. */
template<bool Add>
bool vqrdmlah(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = element_bits(x);
	const bool scalar = o.scalar_form;
	const i128 scalar_value = scalar
		? lane_value(lane(x.d_reg(static_cast<unsigned>(o.m)), static_cast<unsigned>(o.index_value), bits),
			  bits, true)
		: i128{0};
	QcTally qc;
	const bool completed = lanewise(x, [&](Raw n, Raw m, Raw d) {
		const i128 operand1 = lane_value(n, bits, true);
		const i128 operand2 = scalar ? scalar_value : lane_value(m, bits, true);
		const i128 operand3 = lane_value(d, bits, true) * (i128{1} << bits);
		const i128 product = 2 * (operand1 * operand2);
		return qc.take(sat_q(
			shift_right(Add ? operand3 + product : operand3 - product, bits, true), bits, false));
	});
	return qc.finish(x, completed);
}

[[nodiscard]] i128 byte_value(Raw source, unsigned index, bool is_unsigned) noexcept
{
	return lane_value(lane(source, index, 8), 8, !is_unsigned);
}

/* The dot product forms: each 32-bit element accumulates the dot product of
 * four bytes of n and four bytes of m (of the one 32-bit group of D(m) the
 * index names, for the by-element forms). */
bool dot_product(Exec &x, bool op1_unsigned, bool op2_unsigned, bool by_element, unsigned index)
{
	const uint64_t element_source = by_element ? x.d_reg(static_cast<unsigned>(x.o.m)) : uint64_t{0};
	return for_each_register(x, [&](Raw n, Raw m, Raw d) -> Raw {
		const Raw second = by_element ? element_source : m;
		Raw out = 0;
		for (unsigned e = 0; e < 2U; ++e) {
			i128 sum = 0;
			for (unsigned i = 0; i < 4U; ++i)
				sum += byte_value(n, 4U * e + i, op1_unsigned)
					* byte_value(second, (by_element ? 4U * index : 4U * e) + i, op2_unsigned);
			out = with_lane(out, e, 32, lane(d, e, 32) + static_cast<uint64_t>(sum));
		}
		return out;
	});
}

bool vsdot(Exec &x)
{
	return dot_product(x, !x.o.signed_, !x.o.signed_, false, 0);
}
bool vsdot_s(Exec &x)
{
	return dot_product(x, !x.o.signed_, !x.o.signed_, true, static_cast<unsigned>(x.o.index_value));
}
bool vusdot(Exec &x)
{
	return dot_product(x, true, false, false, 0);
}
/* VUSDOT and VSUDOT by element: the signedness is the form's. */
bool vusdot_s(Exec &x)
{
	return dot_product(x, x.o.op1_unsigned, x.o.op2_unsigned, true, static_cast<unsigned>(x.o.i));
}

/* VSMMLA, VUMMLA and VUSMMLA: a 2x8 by 8x2 matrix product of bytes,
 * accumulated into a 2x2 matrix of 32-bit elements. */
bool matrix_multiply(Exec &x)
{
	const Operands &o = x.o;
	const u128 operand1 = q_reg(x, static_cast<unsigned>(o.n));
	const u128 operand2 = q_reg(x, static_cast<unsigned>(o.m));
	u128 result = q_reg(x, static_cast<unsigned>(o.d));
	for (unsigned i = 0; i < 2U; ++i) {
		for (unsigned j = 0; j < 2U; ++j) {
			i128 sum = 0;
			for (unsigned k = 0; k < 8U; ++k) {
				const i128 element1 =
					lane_value(lane128(operand1, 8U * i + k, 8), 8, !o.op1_unsigned);
				const i128 element2 =
					lane_value(lane128(operand2, 8U * j + k, 8), 8, !o.op2_unsigned);
				sum += element1 * element2;
			}
			result = with_lane128(result, 2U * i + j, 32,
				lane128(result, 2U * i + j, 32) + static_cast<uint64_t>(sum));
		}
	}
	set_q_reg(x, static_cast<unsigned>(o.d), result);
	return true;
}

/* CRC32 and CRC32C of the low 8, 16 or 32 bits of Rm into the accumulator
 * Rn: the reflected polynomial divides the bits out one at a time. */
bool crc32(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.size);
	const uint32_t polynomial = o.crc32c ? 0x82f6'3b78U : 0xedb8'8320U;
	const uint32_t input = x.reg(o.m) & static_cast<uint32_t>(lane_mask(bits));
	uint32_t crc = x.reg(o.n) ^ input;
	for (unsigned bit = 0; bit < bits; ++bit)
		crc = crc >> 1 ^ ((crc & 1U) != 0U ? polynomial : 0U);
	x.set_reg(o.d, crc);
	return true;
}

constexpr VectorEntry entries[] = {
	{"VQRDMLAH", vqrdmlah<true>}, {"VQRDMLSH", vqrdmlah<false>},
	{"VSDOT", vsdot}, {"VUDOT", vsdot}, {"VSDOT_s", vsdot_s}, {"VUDOT_s", vsdot_s},
	{"VUSDOT", vusdot}, {"VUSDOT_s", vusdot_s}, {"VSUDOT_s", vusdot_s},
	{"VSMMLA", matrix_multiply}, {"VUMMLA", matrix_multiply}, {"VUSMMLA", matrix_multiply},
	{"CRC32B", crc32}, {"CRC32H", crc32}, {"CRC32W", crc32},
	{"CRC32CB", crc32}, {"CRC32CH", crc32}, {"CRC32CW", crc32},
};

} // namespace

std::span<const VectorEntry> armv8_integer_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
