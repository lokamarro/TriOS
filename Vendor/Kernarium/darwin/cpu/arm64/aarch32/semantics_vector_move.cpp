// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's VFP and Advanced SIMD moves, bitwise operations,
// permutations and FPSCR transfers: VMOV in all its forms, VMRS and VMSR,
// VDUP, the bitwise group, VEXT, VREVn, VTRN, VUZP, VZIP, VSWP and VTBL.

#include "cpu/arm64/aarch32/semantics_vector.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

bool vand(Exec &x)
{
	return for_each_register(x, [](auto n, auto m, auto) { return n & m; });
}
bool vbic_r(Exec &x)
{
	return for_each_register(x, [](auto n, auto m, auto) { return n & ~m; });
}
bool vorr_r(Exec &x)
{
	return for_each_register(x, [](auto n, auto m, auto) { return n | m; });
}
bool vorn(Exec &x)
{
	return for_each_register(x, [](auto n, auto m, auto) { return n | ~m; });
}
bool veor(Exec &x)
{
	return for_each_register(x, [](auto n, auto m, auto) { return n ^ m; });
}
bool vmvn_r(Exec &x)
{
	return for_each_register(x, [](auto, auto m, auto) { return ~m; });
}

/* VBSL, VBIT and VBIF. */
bool vbit_ops(Exec &x)
{
	switch (x.o.operation) {
	case VBitOps::VBIF:
		return for_each_register(x, [](auto n, auto m, auto d) { return (d & m) | (n & ~m); });
	case VBitOps::VBIT:
		return for_each_register(x, [](auto n, auto m, auto d) { return (n & m) | (d & ~m); });
	default:
		return for_each_register(x, [](auto n, auto m, auto d) { return (n & d) | (m & ~d); });
	}
}

/* The immediate forms: AdvSIMDExpandImm() is in the operands. */
bool vbic_i(Exec &x)
{
	const std::uint64_t imm = x.o.imm64;
	return for_each_register(x, [imm](auto, auto, auto d) { return d & ~imm; });
}
bool vorr_i(Exec &x)
{
	const std::uint64_t imm = x.o.imm64;
	return for_each_register(x, [imm](auto, auto, auto d) { return d | imm; });
}
bool vmvn_i(Exec &x)
{
	const std::uint64_t imm = x.o.imm64;
	return for_each_register(x, [imm](auto, auto, auto) { return ~imm; });
}

/* VMOV (immediate): Advanced SIMD fills whole registers, VFP one S or D. */
bool vmov_i(Exec &x)
{
	const Operands &o = x.o;
	const unsigned d = static_cast<unsigned>(o.d);
	if (o.esize <= 32) {
		x.set_s_reg(d, static_cast<std::uint32_t>(o.imm));
		return true;
	}
	for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r)
		x.set_d_reg(d + r, o.imm);
	return true;
}

/* VMOV (register), VFP: a copy of one S or D register. */
bool vmov_r(Exec &x)
{
	const Operands &o = x.o;
	if (o.single_register)
		x.set_s_reg(static_cast<unsigned>(o.d), x.s_reg(static_cast<unsigned>(o.m)));
	else
		x.set_d_reg(static_cast<unsigned>(o.d), x.d_reg(static_cast<unsigned>(o.m)));
	return true;
}

/* VSWP. With d == m the result is UNKNOWN: the registers are left alone. */
bool vswp(Exec &x)
{
	const Operands &o = x.o;
	if (o.d == o.m)
		return true;
	for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r) {
		const std::uint64_t d = x.d_reg(static_cast<unsigned>(o.d) + r);
		const std::uint64_t m = x.d_reg(static_cast<unsigned>(o.m) + r);
		x.set_d_reg(static_cast<unsigned>(o.d) + r, m);
		x.set_d_reg(static_cast<unsigned>(o.m) + r, d);
	}
	return true;
}

/* VMOV between an Arm register and a scalar (a D register element), an S
 * register, a pair of S registers, or a D register. */
bool vmov_scalar_to_core(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const std::uint64_t value =
		lane(x.d_reg(static_cast<unsigned>(o.n)), static_cast<unsigned>(o.index_value), bits);
	x.set_reg(o.t, static_cast<std::uint32_t>(
		o.unsigned_ ? value : lane_bits(lane_value(value, bits, true), 32)));
	return true;
}
bool vmov_core_to_scalar(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const unsigned d = static_cast<unsigned>(o.d);
	x.set_d_reg(d, with_lane(x.d_reg(d), static_cast<unsigned>(o.index_value), bits, x.reg(o.t)));
	return true;
}
bool vmov_s(Exec &x)
{
	const Operands &o = x.o;
	if (o.to_arm_register)
		x.set_reg(o.t, x.s_reg(static_cast<unsigned>(o.n)));
	else
		x.set_s_reg(static_cast<unsigned>(o.n), x.reg(o.t));
	return true;
}
bool vmov_d(Exec &x)
{
	const Operands &o = x.o;
	const unsigned m = static_cast<unsigned>(o.m);
	if (o.to_arm_registers) {
		const std::uint64_t value = x.d_reg(m);
		x.set_reg(o.t, static_cast<std::uint32_t>(value));
		x.set_reg(o.t2, static_cast<std::uint32_t>(value >> 32));
	} else {
		x.set_d_reg(m, std::uint64_t{x.reg(o.t2)} << 32 | x.reg(o.t));
	}
	return true;
}
bool vmov_ss(Exec &x)
{
	const Operands &o = x.o;
	const unsigned m = static_cast<unsigned>(o.m);
	if (o.to_arm_registers) {
		const std::uint32_t first = x.s_reg(m);
		const std::uint32_t second = x.s_reg(m + 1U);
		x.set_reg(o.t, first);
		x.set_reg(o.t2, second);
	} else {
		x.set_s_reg(m, x.reg(o.t));
		x.set_s_reg(m + 1U, x.reg(o.t2));
	}
	return true;
}

/* VDUP: a core register or a scalar replicated across the lanes. */
bool vdup_fill(Exec &x, std::uint64_t scalar)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	std::uint64_t filled = 0;
	for (unsigned e = 0; e < 64U / bits; ++e)
		filled = with_lane(filled, e, bits, scalar);
	for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, filled);
	return true;
}
bool vdup_r(Exec &x)
{
	return vdup_fill(x, x.reg(x.o.t) & lane_mask(static_cast<unsigned>(x.o.esize)));
}
bool vdup_s(Exec &x)
{
	const unsigned bits = static_cast<unsigned>(x.o.esize);
	return vdup_fill(x, lane(x.d_reg(static_cast<unsigned>(x.o.m)),
				static_cast<unsigned>(x.o.index_value), bits));
}

/* VMRS and VMSR reach FPSCR only. VMRS into R15 copies FPSCR's N, Z, C and V
 * into the APSR. */
bool vmrs(Exec &x)
{
	const Operands &o = x.o;
	if (o.t == 15) {
		x.cpu.nzcv = (x.cpu.nzcv & ~fpscr_nzcv) | (x.cpu.a32_fpscr_nzcv & fpscr_nzcv);
	} else {
		x.set_reg(o.t, fpscr_of(x.cpu));
	}
	return true;
}
bool vmsr(Exec &x)
{
	set_fpscr(x.cpu, x.reg(x.o.t));
	return true;
}

/* VEXT: bytes from the concatenation m:n, starting at `position`. */
bool vext(Exec &x)
{
	const Operands &o = x.o;
	const unsigned position = static_cast<unsigned>(o.position) / 8U;
	std::array<std::uint8_t, 32> bytes{};
	const unsigned regs = o.quadword_operation ? 2U : 1U;
	for (unsigned r = 0; r < regs; ++r) {
		const std::uint64_t n = x.d_reg(static_cast<unsigned>(o.n) + r);
		const std::uint64_t m = x.d_reg(static_cast<unsigned>(o.m) + r);
		std::memcpy(bytes.data() + 8U * r, &n, 8);
		std::memcpy(bytes.data() + 8U * regs + 8U * r, &m, 8);
	}
	for (unsigned r = 0; r < regs; ++r) {
		std::uint64_t value;
		std::memcpy(&value, bytes.data() + position + 8U * r, 8);
		x.set_d_reg(static_cast<unsigned>(o.d) + r, value);
	}
	return true;
}

/* VREV16, VREV32 and VREV64: the elements of each container reversed. */
bool vrev(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const unsigned per_container = static_cast<unsigned>(o.elements_per_container);
	const unsigned containers = static_cast<unsigned>(o.containers);
	std::array<std::uint64_t, 2> results{};
	for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r) {
		const std::uint64_t source = x.d_reg(static_cast<unsigned>(o.m) + r);
		std::uint64_t result = 0;
		unsigned element = 0;
		for (unsigned c = 0; c < containers; ++c) {
			unsigned reversed = element + per_container - 1U;
			for (unsigned e = 0; e < per_container; ++e) {
				result = with_lane(result, reversed, bits, lane(source, element, bits));
				++element;
				--reversed;
			}
		}
		results[r] = result;
	}
	for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, results[r]);
	return true;
}

/* VTRN: the odd elements of d swap with the even elements of m. With d == m
 * the result is UNKNOWN; it is computed from the old values and m is written
 * before d, which is what the corpus's oracle records. */
bool vtrn(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const unsigned half = static_cast<unsigned>(o.elements) / 2U;
	const unsigned regs = static_cast<unsigned>(o.regs);
	if (o.d == o.m && bits == 32U) {
		/* The oracle swaps the words of the register. */
		for (unsigned r = 0; r < regs; ++r) {
			const std::uint64_t value = x.d_reg(static_cast<unsigned>(o.d) + r);
			x.set_d_reg(static_cast<unsigned>(o.d) + r, value << 32 | value >> 32);
		}
		return true;
	}
	std::array<std::uint64_t, 2> d_new{}, m_new{};
	for (unsigned r = 0; r < regs; ++r) {
		const std::uint64_t d_in = x.d_reg(static_cast<unsigned>(o.d) + r);
		const std::uint64_t m_in = x.d_reg(static_cast<unsigned>(o.m) + r);
		d_new[r] = d_in;
		m_new[r] = m_in;
		for (unsigned e = 0; e < half; ++e) {
			d_new[r] = with_lane(d_new[r], 2U * e + 1U, bits, lane(m_in, 2U * e, bits));
			m_new[r] = with_lane(m_new[r], 2U * e, bits, lane(d_in, 2U * e + 1U, bits));
		}
	}
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.m) + r, m_new[r]);
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, d_new[r]);
	return true;
}

/* VZIP and VUZP over a D or a Q register pair. With d == m the result is
 * UNKNOWN; as for VTRN it is computed from the old values with m written
 * before d. */
void load_lanes(Exec &x, unsigned first, unsigned count, std::array<std::uint64_t, 4> &out) noexcept
{
	for (unsigned i = 0; i < count; ++i)
		out[i] = x.d_reg(first + i);
}

bool vzip(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const unsigned regs = o.quadword_operation ? 2U : 1U;
	std::array<std::uint64_t, 4> d{}, m{}, zipped{};
	load_lanes(x, static_cast<unsigned>(o.d), regs, d);
	load_lanes(x, static_cast<unsigned>(o.m), regs, m);
	const unsigned total = 64U * regs / bits;
	for (unsigned e = 0; e < total; ++e) {
		const unsigned source = e * bits;
		const std::uint64_t dv = lane(d[source / 64U], (source % 64U) / bits, bits);
		const std::uint64_t mv = lane(m[source / 64U], (source % 64U) / bits, bits);
		for (unsigned k = 0; k < 2U; ++k) {
			const unsigned target = (2U * e + k) * bits;
			zipped[target / 64U] = with_lane(zipped[target / 64U], (target % 64U) / bits, bits,
				k == 0U ? dv : mv);
		}
	}
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.m) + r, zipped[regs + r]);
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, zipped[r]);
	return true;
}

bool vuzp(Exec &x)
{
	const Operands &o = x.o;
	const unsigned bits = static_cast<unsigned>(o.esize);
	const unsigned regs = o.quadword_operation ? 2U : 1U;
	std::array<std::uint64_t, 4> d{}, m{}, combined{}, evens{}, odds{};
	load_lanes(x, static_cast<unsigned>(o.d), regs, d);
	load_lanes(x, static_cast<unsigned>(o.m), regs, m);
	/* zipped = m:d, with d in the low half. */
	for (unsigned r = 0; r < regs; ++r) {
		combined[r] = d[r];
		combined[regs + r] = m[r];
	}
	const unsigned total = 64U * regs / bits;
	for (unsigned e = 0; e < total; ++e) {
		for (unsigned k = 0; k < 2U; ++k) {
			const unsigned source = (2U * e + k) * bits;
			const std::uint64_t value =
				lane(combined[source / 64U], (source % 64U) / bits, bits);
			const unsigned target = e * bits;
			auto &destination = k == 0U ? evens : odds;
			destination[target / 64U] =
				with_lane(destination[target / 64U], (target % 64U) / bits, bits, value);
		}
	}
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.m) + r, odds[r]);
	for (unsigned r = 0; r < regs; ++r)
		x.set_d_reg(static_cast<unsigned>(o.d) + r, evens[r]);
	return true;
}

/* VTBL and VTBX: a byte table of one to four D registers. */
bool vtbl(Exec &x)
{
	const Operands &o = x.o;
	const unsigned length = static_cast<unsigned>(o.length);
	std::array<std::uint8_t, 32> table{};
	for (unsigned r = 0; r < length; ++r) {
		const std::uint64_t value = x.d_reg(static_cast<unsigned>(o.n) + r);
		std::memcpy(table.data() + 8U * r, &value, 8);
	}
	const std::uint64_t indexes = x.d_reg(static_cast<unsigned>(o.m));
	std::uint64_t result = o.is_vtbl ? 0U : x.d_reg(static_cast<unsigned>(o.d));
	for (unsigned i = 0; i < 8U; ++i) {
		const unsigned index = lane(indexes, i, 8);
		if (index < 8U * length)
			result = with_lane(result, i, 8, table[index]);
		else if (o.is_vtbl)
			result = with_lane(result, i, 8, 0);
	}
	x.set_d_reg(static_cast<unsigned>(o.d), result);
	return true;
}

constexpr VectorEntry entries[] = {
	{"VAND_r", &vand}, {"VBIC_r", &vbic_r}, {"VORR_r", &vorr_r},
	{"VORN_r", &vorn}, {"VEOR", &veor}, {"VMVN_r", &vmvn_r},
	{"VBIF", &vbit_ops}, {"VBIT", &vbit_ops}, {"VBSL", &vbit_ops},
	{"VBIC_i", &vbic_i}, {"VORR_i", &vorr_i}, {"VMVN_i", &vmvn_i},
	{"VMOV_i", &vmov_i}, {"VMOV_r", &vmov_r}, {"VSWP", &vswp},
	{"VMOV_sr", &vmov_scalar_to_core}, {"VMOV_rs", &vmov_core_to_scalar},
	{"VMOV_s", &vmov_s}, {"VMOV_tos", &vmov_s}, {"VMOV_d", &vmov_d},
	{"VMOV_tod", &vmov_d}, {"VMOV_ss", &vmov_ss}, {"VMOV_toss", &vmov_ss},
	{"VDUP_r", &vdup_r}, {"VDUP_s", &vdup_s}, {"VMRS", &vmrs},
	{"VMSR", &vmsr}, {"VEXT", &vext}, {"VREV16", &vrev}, {"VREV32", &vrev},
	{"VREV64", &vrev}, {"VTRN", &vtrn}, {"VZIP", &vzip}, {"VUZP", &vuzp},
	{"VTBL", &vtbl}, {"VTBX", &vtbl},
};

} // namespace

std::span<const VectorEntry> vector_move_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
