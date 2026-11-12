// SPDX-License-Identifier: GPL-2.0-or-later
//
// The integer families of the AArch32 semantic library: data processing with
// its shifter operand, bit fields, extension and packing, multiplies and
// divides, saturation and the parallel arithmetic (semantics.hpp).

#include "cpu/arm64/aarch32/semantics_internal.hpp"

namespace uml::cpu::arm64::aarch32::detail {

namespace {

constexpr bool is_logical(Op op) noexcept
{
	switch (op) {
	case Op::and_: case Op::bic: case Op::eor: case Op::mov: case Op::mvn:
	case Op::orn: case Op::orr: case Op::teq: case Op::tst:
		return true;
	default:
		return false;
	}
}

/* The second operand of a data-processing form and the carry its shifter or
 * immediate produces (the Shift_C of the registers, the ExpandImm_C of an
 * immediate). */
struct SecondOperand final {
	std::uint32_t value;
	bool carry;
};

SecondOperand second_operand(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.shape) {
	case Shape::reg: {
		const auto shifted = shift_c(x.reg(o.m), o.shift_t,
			static_cast<unsigned>(o.shift_n), x.flag_c());
		return {shifted.value, shifted.carry};
	}
	case Shape::reg_shifted: {
		const unsigned amount = x.reg(o.s) & 0xffU;
		const auto shifted = shift_c(x.reg(o.m), o.shift_t, amount, x.flag_c());
		return {shifted.value, shifted.carry};
	}
	case Shape::immediate:
		break;
	default:
		return {0U, x.flag_c()};
	}
	/* The logical immediate forms carry the 12-bit field and expand it
	 * here, with the carry flag; the others decoded imm32. */
	if (is_logical(info.op) && info.op != Op::mov) {
		const auto expanded = o.a32 ? a32_expand_imm_c(o.imm)
					    : t32_expand_imm_c(o.imm);
		return {static_cast<std::uint32_t>(expanded.imm32),
			expanded.carry_in ? x.flag_c() : expanded.carry != 0U};
	}
	return {static_cast<std::uint32_t>(o.imm32),
		o.carry_flags ? x.flag_c() : o.carry != 0U};
}

} // namespace

bool exec_data_processing(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::movw:
		x.set_reg(o.d, static_cast<std::uint32_t>(o.imm32));
		return true;
	case Op::movt:
		x.set_reg(o.d, (x.reg(o.d) & 0xffffU) | std::uint32_t{o.imm16} << 16);
		return true;
	case Op::adr: {
		const std::uint32_t base = x.pc_aligned();
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const std::uint32_t result = o.add ? base + offset : base - offset;
		if (o.d == static_cast<std::int32_t>(reg_pc))
			x.alu_write_pc(result);
		else
			x.set_reg(o.d, result);
		return true;
	}
	default:
		break;
	}

	const unsigned n = info.sp_base ? reg_sp : static_cast<unsigned>(o.n);
	const bool wants_n = info.op != Op::mov && info.op != Op::mvn;
	const std::uint32_t a = wants_n ? x.reg(n) : 0U;
	const SecondOperand second = second_operand(x, info);
	const std::uint32_t b = second.value;

	std::uint32_t result = 0;
	bool carry = false;
	bool overflow = false;
	bool compare = false;
	/* AddWithCarry() into the result and the carry and overflow flags. */
	const auto add = [&](std::uint32_t first, std::uint32_t second, bool carry_in) {
		const auto sum = add_with_carry(first, second, carry_in);
		result = sum.value; carry = sum.carry; overflow = sum.overflow;
	};
	switch (info.op) {
	case Op::add: case Op::addw: case Op::cmn:
		add(a, b, false); compare = info.op == Op::cmn;
		break;
	case Op::adc:
		add(a, b, x.flag_c());
		break;
	case Op::sub: case Op::subw: case Op::cmp:
		add(a, ~b, true); compare = info.op == Op::cmp;
		break;
	case Op::sbc:
		add(a, ~b, x.flag_c());
		break;
	case Op::rsb:
		add(~a, b, true);
		break;
	case Op::rsc:
		add(~a, b, x.flag_c());
		break;
	case Op::and_: case Op::tst:
		result = a & b; carry = second.carry; compare = info.op == Op::tst;
		break;
	case Op::eor: case Op::teq:
		result = a ^ b; carry = second.carry; compare = info.op == Op::teq;
		break;
	case Op::orr:
		result = a | b; carry = second.carry;
		break;
	case Op::orn:
		result = a | ~b; carry = second.carry;
		break;
	case Op::bic:
		result = a & ~b; carry = second.carry;
		break;
	case Op::mov:
		result = b; carry = second.carry;
		break;
	case Op::mvn:
		result = ~b; carry = second.carry;
		break;
	default:
		return x.raise_fatal();
	}

	if (!compare) {
		if (o.d == static_cast<std::int32_t>(reg_pc))
			x.alu_write_pc(result);
		else
			x.set_reg(o.d, result);
	}
	if (compare || o.setflags) {
		if (is_logical(info.op))
			x.set_nzc(result, carry);
		else
			x.set_nzcv((result >> 31) != 0U, result == 0U, carry, overflow);
	}
	return true;
}

namespace {

[[nodiscard]] constexpr std::int32_t sext8(std::uint32_t value) noexcept
{
	return static_cast<std::int8_t>(value & 0xffU);
}

[[nodiscard]] constexpr std::int32_t sext16(std::uint32_t value) noexcept
{
	return static_cast<std::int16_t>(value & 0xffffU);
}

} // namespace

bool exec_bits_and_extend(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::bfc:
	case Op::bfi: {
		const unsigned lsb = static_cast<unsigned>(o.lsbit);
		const unsigned width = static_cast<unsigned>(o.msbit - o.lsbit) + 1U;
		const std::uint32_t field = width >= 32U ? 0xffff'ffffU
							 : (1U << width) - 1U;
		const std::uint32_t mask = field << lsb;
		const std::uint32_t source =
			info.op == Op::bfi ? (x.reg(o.n) << lsb) & mask : 0U;
		x.set_reg(o.d, (x.reg(o.d) & ~mask) | source);
		return true;
	}
	case Op::sbfx:
	case Op::ubfx: {
		const unsigned lsb = static_cast<unsigned>(o.lsbit);
		const unsigned width = static_cast<unsigned>(o.widthminus1) + 1U;
		const std::uint32_t field = width >= 32U ? 0xffff'ffffU
							 : (1U << width) - 1U;
		std::uint32_t value = x.reg(o.n) >> lsb & field;
		if (info.op == Op::sbfx && width < 32U && (value >> (width - 1U) & 1U) != 0U)
			value |= ~field;
		x.set_reg(o.d, value);
		return true;
	}
	case Op::clz:
		x.set_reg(o.d, static_cast<std::uint32_t>(std::countl_zero(x.reg(o.m))));
		return true;
	case Op::rbit:
		x.set_reg(o.d, __builtin_bitreverse32(x.reg(o.m)));
		return true;
	case Op::rev:
		x.set_reg(o.d, __builtin_bswap32(x.reg(o.m)));
		return true;
	case Op::rev16: {
		const std::uint32_t value = x.reg(o.m);
		x.set_reg(o.d, (value >> 8 & 0x00ff'00ffU) | (value & 0x00ff'00ffU) << 8);
		return true;
	}
	case Op::revsh: {
		const std::uint32_t value = x.reg(o.m);
		const std::uint32_t swapped = (value & 0xffU) << 8 | (value >> 8 & 0xffU);
		x.set_reg(o.d, static_cast<std::uint32_t>(sext16(swapped)));
		return true;
	}
	case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth:
	case Op::sxtab: case Op::sxtah: case Op::uxtab: case Op::uxtah:
	case Op::sxtb16: case Op::uxtb16: case Op::sxtab16: case Op::uxtab16: {
		const std::uint32_t rotated = std::rotr(x.reg(o.m), o.rotation);
		const bool accumulate = info.op == Op::sxtab || info.op == Op::sxtah
			|| info.op == Op::uxtab || info.op == Op::uxtah
			|| info.op == Op::sxtab16 || info.op == Op::uxtab16;
		const std::uint32_t addend = accumulate ? x.reg(o.n) : 0U;
		std::uint32_t result;
		switch (info.op) {
		case Op::sxtb: case Op::sxtab:
			result = addend + static_cast<std::uint32_t>(sext8(rotated));
			break;
		case Op::uxtb: case Op::uxtab:
			result = addend + (rotated & 0xffU);
			break;
		case Op::sxth: case Op::sxtah:
			result = addend + static_cast<std::uint32_t>(sext16(rotated));
			break;
		case Op::uxth: case Op::uxtah:
			result = addend + (rotated & 0xffffU);
			break;
		case Op::sxtb16: case Op::sxtab16: {
			const std::uint32_t low = (addend + static_cast<std::uint32_t>(sext8(rotated))) & 0xffffU;
			const std::uint32_t high = ((addend >> 16) + static_cast<std::uint32_t>(sext8(rotated >> 16))) & 0xffffU;
			result = high << 16 | low;
			break;
		}
		default: {
			const std::uint32_t low = ((addend & 0xffffU) + (rotated & 0xffU)) & 0xffffU;
			const std::uint32_t high = ((addend >> 16) + (rotated >> 16 & 0xffU)) & 0xffffU;
			result = high << 16 | low;
			break;
		}
		}
		x.set_reg(o.d, result);
		return true;
	}
	case Op::pkh: {
		const std::uint32_t operand2 = shift_c(x.reg(o.m), o.shift_t,
			static_cast<unsigned>(o.shift_n), x.flag_c()).value;
		const std::uint32_t n = x.reg(o.n);
		x.set_reg(o.d, o.tbform ? (n & 0xffff'0000U) | (operand2 & 0xffffU)
					: (operand2 & 0xffff'0000U) | (n & 0xffffU));
		return true;
	}
	case Op::sel: {
		const std::uint32_t n = x.reg(o.n);
		const std::uint32_t m = x.reg(o.m);
		x.set_reg(o.d, (n & x.cpu.a32_ge_mask) | (m & ~x.cpu.a32_ge_mask));
		return true;
	}
	default:
		return x.raise_fatal();
	}
}

namespace {

/* The 16-bit half of a register selected by an index: 0 bottom, 1 top. */
[[nodiscard]] constexpr std::int32_t half(std::uint32_t value, bool top) noexcept
{
	return sext16(top ? value >> 16 : value);
}

} // namespace

bool exec_multiply(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::mul: {
		const std::uint32_t result = x.reg(o.n) * x.reg(o.m);
		x.set_reg(o.d, result);
		if (o.setflags)
			x.set_nzc(result, x.flag_c());
		return true;
	}
	case Op::mla:
	case Op::mls: {
		const std::uint32_t product = x.reg(o.n) * x.reg(o.m);
		const std::uint32_t result = info.op == Op::mla ? product + x.reg(o.a)
								  : x.reg(o.a) - product;
		x.set_reg(o.d, result);
		if (o.setflags)
			x.set_nzc(result, x.flag_c());
		return true;
	}
	case Op::umull: case Op::umlal: case Op::smull: case Op::smlal: case Op::umaal: {
		const bool is_signed = info.op == Op::smull || info.op == Op::smlal;
		const std::uint64_t product = is_signed
			? static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(x.reg(o.n))}
				* std::int64_t{static_cast<std::int32_t>(x.reg(o.m))})
			: std::uint64_t{x.reg(o.n)} * std::uint64_t{x.reg(o.m)};
		std::uint64_t result = product;
		if (info.op == Op::umlal || info.op == Op::smlal)
			result += std::uint64_t{x.reg(o.dHi)} << 32 | x.reg(o.dLo);
		else if (info.op == Op::umaal)
			result += std::uint64_t{x.reg(o.dHi)} + std::uint64_t{x.reg(o.dLo)};
		x.set_reg(o.dLo, static_cast<std::uint32_t>(result));
		x.set_reg(o.dHi, static_cast<std::uint32_t>(result >> 32));
		if (o.setflags)
			x.set_nzcv((result >> 63) != 0U, result == 0U, x.flag_c(), x.flag_v());
		return true;
	}
	case Op::sdiv: {
		const auto n = static_cast<std::int32_t>(x.reg(o.n));
		const auto m = static_cast<std::int32_t>(x.reg(o.m));
		std::int32_t result;
		if (m == 0)
			result = 0;
		else if (n == INT32_MIN && m == -1)
			result = INT32_MIN;
		else
			result = n / m;
		x.set_reg(o.d, static_cast<std::uint32_t>(result));
		return true;
	}
	case Op::udiv: {
		const std::uint32_t m = x.reg(o.m);
		x.set_reg(o.d, m == 0U ? 0U : x.reg(o.n) / m);
		return true;
	}
	case Op::smla_xy: case Op::smul_xy: {
		const std::int64_t product = std::int64_t{half(x.reg(o.n), o.n_index != 0)}
			* std::int64_t{half(x.reg(o.m), o.m_index != 0)};
		if (info.op == Op::smul_xy) {
			x.set_reg(o.d, static_cast<std::uint32_t>(product));
			return true;
		}
		const std::int64_t result =
			product + std::int64_t{static_cast<std::int32_t>(x.reg(o.a))};
		x.set_reg(o.d, static_cast<std::uint32_t>(result));
		if (result != std::int64_t{static_cast<std::int32_t>(result)})
			x.set_q();
		return true;
	}
	case Op::smlal_xy: {
		const std::int64_t product = std::int64_t{half(x.reg(o.n), o.n_index != 0)}
			* std::int64_t{half(x.reg(o.m), o.m_index != 0)};
		const std::uint64_t result = static_cast<std::uint64_t>(product)
			+ (std::uint64_t{x.reg(o.dHi)} << 32 | x.reg(o.dLo));
		x.set_reg(o.dLo, static_cast<std::uint32_t>(result));
		x.set_reg(o.dHi, static_cast<std::uint32_t>(result >> 32));
		return true;
	}
	case Op::smlaw: case Op::smulw: {
		const std::int64_t product = std::int64_t{static_cast<std::int32_t>(x.reg(o.n))}
			* std::int64_t{half(x.reg(o.m), o.m_high)};
		if (info.op == Op::smulw) {
			x.set_reg(o.d, static_cast<std::uint32_t>(product >> 16));
			return true;
		}
		const std::int64_t result = product
			+ (std::int64_t{static_cast<std::int32_t>(x.reg(o.a))} << 16);
		x.set_reg(o.d, static_cast<std::uint32_t>(result >> 16));
		if ((result >> 16) != std::int64_t{static_cast<std::int32_t>(result >> 16)})
			x.set_q();
		return true;
	}
	case Op::smlad: case Op::smlsd: case Op::smuad: case Op::smusd:
	case Op::smlald: case Op::smlsld: {
		const std::uint32_t n = x.reg(o.n);
		std::uint32_t m = x.reg(o.m);
		if (o.m_swap)
			m = std::rotr(m, 16);
		const std::int64_t product1 = std::int64_t{sext16(n)} * std::int64_t{sext16(m)};
		const std::int64_t product2 =
			std::int64_t{sext16(n >> 16)} * std::int64_t{sext16(m >> 16)};
		const bool subtract = info.op == Op::smlsd || info.op == Op::smusd
			|| info.op == Op::smlsld;
		const std::int64_t sum = subtract ? product1 - product2 : product1 + product2;
		if (info.op == Op::smlald || info.op == Op::smlsld) {
			const std::uint64_t result = static_cast<std::uint64_t>(sum)
				+ (std::uint64_t{x.reg(o.dHi)} << 32 | x.reg(o.dLo));
			x.set_reg(o.dLo, static_cast<std::uint32_t>(result));
			x.set_reg(o.dHi, static_cast<std::uint32_t>(result >> 32));
			return true;
		}
		const bool accumulates = info.op == Op::smlad || info.op == Op::smlsd;
		const std::int64_t result = accumulates
			? sum + std::int64_t{static_cast<std::int32_t>(x.reg(o.a))} : sum;
		x.set_reg(o.d, static_cast<std::uint32_t>(result));
		if (result != std::int64_t{static_cast<std::int32_t>(result)})
			x.set_q();
		return true;
	}
	case Op::smmla: case Op::smmls: case Op::smmul: {
		std::uint64_t product = static_cast<std::uint64_t>(
			std::int64_t{static_cast<std::int32_t>(x.reg(o.n))}
			* std::int64_t{static_cast<std::int32_t>(x.reg(o.m))});
		std::uint64_t result;
		if (info.op == Op::smmul) {
			result = product;
		} else {
			const std::uint64_t addend = std::uint64_t{x.reg(o.a)} << 32;
			result = info.op == Op::smmla ? addend + product : addend - product;
		}
		if (o.round)
			result += UINT64_C(0x80000000);
		x.set_reg(o.d, static_cast<std::uint32_t>(result >> 32));
		return true;
	}
	case Op::usad8: case Op::usada8: {
		const std::uint32_t n = x.reg(o.n);
		const std::uint32_t m = x.reg(o.m);
		std::uint32_t sum = 0;
		for (unsigned lane = 0; lane < 4U; ++lane) {
			const std::uint32_t p = n >> (8U * lane) & 0xffU;
			const std::uint32_t q = m >> (8U * lane) & 0xffU;
			sum += p > q ? p - q : q - p;
		}
		if (info.op == Op::usada8)
			sum += x.reg(o.a);
		x.set_reg(o.d, sum);
		return true;
	}
	default:
		return x.raise_fatal();
	}
}

namespace {

/* One lane of a parallel add or subtract: the exact result of the operation
 * on lane values extended to integers, and the lane as the destination
 * sees it for the mode. No caller reads `exact`; it stays because removing
 * it made the parallel forms about 2 percent slower. */
struct ParallelLane final {
	std::int32_t exact;
	std::uint32_t out;
	bool ge;
};

[[nodiscard]] ParallelLane parallel_lane(std::int32_t a, std::int32_t b, bool subtract,
	ParallelMode mode, unsigned width) noexcept
{
	const std::int32_t exact = subtract ? a - b : a + b;
	const std::uint32_t mask = (1U << width) - 1U;
	ParallelLane lane{exact, static_cast<std::uint32_t>(exact) & mask, false};
	switch (mode) {
	case ParallelMode::signed_wrap:
		lane.ge = exact >= 0;
		break;
	case ParallelMode::unsigned_wrap:
		lane.ge = subtract ? exact >= 0 : exact >= (1 << width);
		break;
	case ParallelMode::saturating:
		lane.out = static_cast<std::uint32_t>(signed_sat(exact, width).value) & mask;
		break;
	case ParallelMode::unsigned_saturating:
		lane.out = static_cast<std::uint32_t>(unsigned_sat(exact, width).value) & mask;
		break;
	case ParallelMode::signed_halving:
	case ParallelMode::unsigned_halving:
		lane.out = static_cast<std::uint32_t>(exact >> 1) & mask;
		break;
	}
	return lane;
}

} // namespace

bool exec_saturate_and_parallel(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub: {
		const std::int64_t n = static_cast<std::int32_t>(x.reg(o.n));
		const std::int64_t m = static_cast<std::int32_t>(x.reg(o.m));
		bool saturated = false;
		std::int64_t operand = n;
		if (info.op == Op::qdadd || info.op == Op::qdsub) {
			const auto doubled = signed_sat(2 * n, 32);
			saturated = doubled.saturated;
			operand = doubled.value;
		}
		const bool subtract = info.op == Op::qsub || info.op == Op::qdsub;
		const auto sum = signed_sat(subtract ? m - operand : m + operand, 32);
		x.set_reg(o.d, static_cast<std::uint32_t>(sum.value));
		if (saturated || sum.saturated)
			x.set_q();
		return true;
	}
	case Op::ssat: case Op::usat: {
		const auto operand = static_cast<std::int32_t>(shift_c(x.reg(o.n), o.shift_t,
			static_cast<unsigned>(o.shift_n), x.flag_c()).value);
		const unsigned bits = static_cast<unsigned>(o.saturate_to);
		const auto result = info.op == Op::ssat ? signed_sat(operand, bits) : unsigned_sat(operand, bits);
		x.set_reg(o.d, static_cast<std::uint32_t>(result.value));
		if (result.saturated)
			x.set_q();
		return true;
	}
	case Op::ssat16: case Op::usat16: {
		const std::uint32_t n = x.reg(o.n);
		const unsigned bits = static_cast<unsigned>(o.saturate_to);
		std::uint32_t result = 0;
		bool saturated = false;
		for (unsigned lane = 0; lane < 2U; ++lane) {
			const std::int32_t value = sext16(n >> (16U * lane));
			const auto lane_result = info.op == Op::ssat16 ? signed_sat(value, bits)
								      : unsigned_sat(value, bits);
			saturated |= lane_result.saturated;
			result |= (static_cast<std::uint32_t>(lane_result.value) & 0xffffU) << (16U * lane);
		}
		x.set_reg(o.d, result);
		if (saturated)
			x.set_q();
		return true;
	}
	case Op::parallel: {
		const std::uint32_t n = x.reg(o.n);
		const std::uint32_t m = x.reg(o.m);
		const bool is_signed = info.mode == ParallelMode::signed_wrap
			|| info.mode == ParallelMode::saturating
			|| info.mode == ParallelMode::signed_halving;
		const unsigned width = info.kind == ParallelKind::add8
			|| info.kind == ParallelKind::sub8 ? 8U : 16U;
		const unsigned lanes = 32U / width;
		const auto element = [&](std::uint32_t value, unsigned lane) {
			const std::uint32_t raw = value >> (width * lane) & ((1U << width) - 1U);
			if (!is_signed)
				return static_cast<std::int32_t>(raw);
			return width == 8U ? sext8(raw) : sext16(raw);
		};
		std::uint32_t result = 0;
		std::uint32_t ge = 0;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			bool subtract;
			unsigned m_lane = lane;
			switch (info.kind) {
			case ParallelKind::add16: case ParallelKind::add8:
				subtract = false;
				break;
			case ParallelKind::sub16: case ParallelKind::sub8:
				subtract = true;
				break;
			case ParallelKind::asx:
				subtract = lane == 0U;
				m_lane = 1U - lane;
				break;
			default:
				subtract = lane == 1U;
				m_lane = 1U - lane;
				break;
			}
			const auto out = parallel_lane(element(n, lane), element(m, m_lane),
				subtract, info.mode, width);
			result |= out.out << (width * lane);
			if (out.ge)
				ge |= ((1U << (width / 8U)) - 1U) << (lane * (width / 8U));
		}
		x.set_reg(o.d, result);
		if (info.mode == ParallelMode::signed_wrap || info.mode == ParallelMode::unsigned_wrap)
			for (unsigned lane = 0; lane < 4U; ++lane)
				x.set_ge_lane(lane, (ge >> lane & 1U) != 0U);
		return true;
	}
	default:
		return x.raise_fatal();
	}
}

} // namespace uml::cpu::arm64::aarch32::detail
