// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_dependency_reference.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace arm64_gadget_dependency_reference {
namespace {

using uml::cpu::arm64::CpuState;
using uml::cpu::arm64::VectorRegister;

constexpr std::uint32_t field(std::uint32_t word, unsigned low, unsigned width)
{
	return (word >> low) & ((1U << width) - 1U);
}

constexpr std::uint64_t ones(unsigned count)
{
	return count >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1U;
}

constexpr std::int64_t sign_extend(std::uint64_t value, unsigned width)
{
	const auto shift = 64U - width;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

constexpr unsigned operand_width(std::uint32_t word)
{
	return field(word, 31, 1) != 0 ? 64U : 32U;
}

/* Register 31 is the zero register unless the encoding selects SP. */
std::uint64_t read(const CpuState &cpu, unsigned reg, unsigned width,
	bool stack_pointer = false)
{
	const auto value = reg < cpu.registers.size() ? cpu.registers[reg] :
		stack_pointer ? cpu.sp : 0U;
	return value & ones(width);
}

/* A 32-bit result is zero-extended into the 64-bit register. */
void write(CpuState &cpu, unsigned reg, std::uint64_t value, unsigned width,
	bool stack_pointer = false)
{
	value &= ones(width);
	if (reg < cpu.registers.size())
		cpu.registers[reg] = value;
	else if (stack_pointer)
		cpu.sp = value;
}

constexpr std::uint32_t flags(bool n, bool z, bool c, bool v)
{
	return (std::uint32_t{n} << 31) | (std::uint32_t{z} << 30) |
		(std::uint32_t{c} << 29) | (std::uint32_t{v} << 28);
}

std::uint32_t result_flags(std::uint64_t result, unsigned width)
{
	return flags((result >> (width - 1U) & 1U) != 0, result == 0, false, false);
}

struct Sum final {
	std::uint64_t result;
	std::uint32_t nzcv;
};

/* AddWithCarry() */
Sum add_with_carry(std::uint64_t x, std::uint64_t y, bool carry, unsigned width)
{
	x &= ones(width);
	y &= ones(width);
	const auto unsigned_sum = static_cast<unsigned __int128>(x) + y + carry;
	const auto signed_sum = static_cast<__int128>(sign_extend(x, width)) +
		sign_extend(y, width) + carry;
	const auto result = static_cast<std::uint64_t>(unsigned_sum) & ones(width);
	return {result, flags((result >> (width - 1U) & 1U) != 0, result == 0,
		result != unsigned_sum, sign_extend(result, width) != signed_sum)};
}

/* ConditionHolds() */
bool condition_holds(std::uint32_t nzcv, unsigned condition)
{
	const bool n = (nzcv >> 31 & 1U) != 0, z = (nzcv >> 30 & 1U) != 0;
	const bool c = (nzcv >> 29 & 1U) != 0, v = (nzcv >> 28 & 1U) != 0;
	bool result{};
	switch (condition >> 1) {
	case 0: result = z; break;
	case 1: result = c; break;
	case 2: result = n; break;
	case 3: result = v; break;
	case 4: result = c && !z; break;
	case 5: result = n == v; break;
	case 6: result = n == v && !z; break;
	default: result = true; break;
	}
	return (condition & 1U) != 0 && condition != 15 ? !result : result;
}

enum ShiftType : unsigned { lsl, lsr, asr, ror };

/* ShiftReg() for an amount below the operand width. */
std::uint64_t shift(std::uint64_t value, unsigned type, unsigned amount, unsigned width)
{
	value &= ones(width);
	if (amount == 0)
		return value;
	switch (type) {
	case lsl: return (value << amount) & ones(width);
	case lsr: return value >> amount;
	case asr:
		return static_cast<std::uint64_t>(sign_extend(value, width) >> amount) &
			ones(width);
	default: return ((value >> amount) | (value << (width - amount))) & ones(width);
	}
}

/* ExtendReg(): extend the low bits of the register, then shift left. */
std::uint64_t extend(std::uint64_t value, unsigned option, unsigned amount, unsigned width)
{
	const auto length = std::min(8U << (option & 3U), width - amount);
	auto extended = value & ones(length);
	if ((option & 4U) != 0)
		extended = static_cast<std::uint64_t>(sign_extend(extended, length));
	return (extended << amount) & ones(width);
}

struct BitMasks final {
	std::uint64_t wmask;
	std::uint64_t tmask;
};

/* DecodeBitMasks() */
std::optional<BitMasks> decode_bit_masks(unsigned n, unsigned imms, unsigned immr,
	bool immediate, unsigned width)
{
	const auto length = static_cast<int>(std::bit_width((n << 6) | (~imms & 0x3fU))) - 1;
	if (length < 1)
		return std::nullopt;
	const auto element = 1U << length;
	if (element > width)
		return std::nullopt;
	const auto levels = element - 1U;
	if (immediate && (imms & levels) == levels)
		return std::nullopt;
	const auto s = imms & levels, r = immr & levels;
	const auto welem = ones(s + 1U);
	const auto rotated = r == 0 ? welem :
		((welem >> r) | (welem << (element - r))) & ones(element);
	const auto telem = ones(((s - r) & levels) + 1U);
	BitMasks masks{};
	for (unsigned position = 0; position < width; position += element) {
		masks.wmask |= rotated << position;
		masks.tmask |= telem << position;
	}
	return masks;
}

std::uint64_t lane(const VectorRegister &reg, unsigned index, unsigned bytes)
{
	std::uint64_t value{};
	for (unsigned byte = 0; byte < bytes; ++byte)
		value |= std::uint64_t{std::to_integer<std::uint8_t>(
			reg.bytes[index * bytes + byte])} << (8U * byte);
	return value;
}

void set_lane(VectorRegister &reg, unsigned index, unsigned bytes, std::uint64_t value)
{
	for (unsigned byte = 0; byte < bytes; ++byte)
		reg.bytes[index * bytes + byte] = static_cast<std::byte>(value >> (8U * byte) & 0xffU);
}

bool add_subtract_immediate(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const bool subtract = field(word, 30, 1) != 0, set_flags = field(word, 29, 1) != 0;
	const auto immediate = std::uint64_t{field(word, 10, 12)} << (12U * field(word, 22, 1));
	const auto sum = add_with_carry(read(cpu, field(word, 5, 5), width, true),
		subtract ? ~immediate : immediate, subtract, width);
	if (set_flags)
		cpu.nzcv = sum.nzcv;
	write(cpu, field(word, 0, 5), sum.result, width, !set_flags);
	return true;
}

bool add_subtract_shifted(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto type = field(word, 22, 2), amount = field(word, 10, 6);
	if (type == ror || amount >= width)
		return false;
	const bool subtract = field(word, 30, 1) != 0, set_flags = field(word, 29, 1) != 0;
	const auto operand = shift(read(cpu, field(word, 16, 5), width), type, amount, width);
	const auto sum = add_with_carry(read(cpu, field(word, 5, 5), width),
		subtract ? ~operand : operand, subtract, width);
	if (set_flags)
		cpu.nzcv = sum.nzcv;
	write(cpu, field(word, 0, 5), sum.result, width);
	return true;
}

bool add_subtract_extended(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto amount = field(word, 10, 3);
	if (amount > 4)
		return false;
	const bool subtract = field(word, 30, 1) != 0, set_flags = field(word, 29, 1) != 0;
	const auto operand = extend(read(cpu, field(word, 16, 5), width),
		field(word, 13, 3), amount, width);
	const auto sum = add_with_carry(read(cpu, field(word, 5, 5), width, true),
		subtract ? ~operand : operand, subtract, width);
	if (set_flags)
		cpu.nzcv = sum.nzcv;
	write(cpu, field(word, 0, 5), sum.result, width, !set_flags);
	return true;
}

bool add_subtract_carry(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const bool subtract = field(word, 30, 1) != 0;
	const auto operand = read(cpu, field(word, 16, 5), width);
	const auto sum = add_with_carry(read(cpu, field(word, 5, 5), width),
		subtract ? ~operand : operand, (cpu.nzcv >> 29 & 1U) != 0, width);
	if (field(word, 29, 1) != 0)
		cpu.nzcv = sum.nzcv;
	write(cpu, field(word, 0, 5), sum.result, width);
	return true;
}

/* AND, ORR, EOR and ANDS share their operation and flag rules. */
void logical(CpuState &cpu, std::uint32_t word, std::uint64_t operand,
	unsigned width, bool stack_pointer)
{
	const auto left = read(cpu, field(word, 5, 5), width);
	const auto operation = field(word, 29, 2);
	const auto result = operation == 1 ? left | operand :
		operation == 2 ? left ^ operand : left & operand;
	if (operation == 3)
		cpu.nzcv = result_flags(result, width);
	write(cpu, field(word, 0, 5), result, width, stack_pointer && operation != 3);
}

bool logical_shifted(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto amount = field(word, 10, 6);
	if (amount >= width)
		return false;
	auto operand = shift(read(cpu, field(word, 16, 5), width), field(word, 22, 2),
		amount, width);
	if (field(word, 21, 1) != 0)
		operand = ~operand & ones(width);
	logical(cpu, word, operand, width, false);
	return true;
}

bool logical_immediate(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto n = field(word, 22, 1);
	if (width == 32 && n != 0)
		return false;
	const auto masks = decode_bit_masks(n, field(word, 10, 6), field(word, 16, 6),
		true, width);
	if (!masks)
		return false;
	logical(cpu, word, masks->wmask, width, true);
	return true;
}

bool move_wide(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto operation = field(word, 29, 2), half = field(word, 21, 2);
	if (operation == 1 || 16U * half >= width)
		return false;
	const auto position = 16U * half;
	const auto immediate = std::uint64_t{field(word, 5, 16)} << position;
	const auto rd = field(word, 0, 5);
	const auto result = operation == 0 ? ~immediate : operation == 2 ? immediate :
		(read(cpu, rd, width) & ~(std::uint64_t{0xffff} << position)) | immediate;
	write(cpu, rd, result, width);
	return true;
}

/* SBFM, BFM and UBFM in their DecodeBitMasks() form. */
bool bitfield(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto operation = field(word, 29, 2), n = field(word, 22, 1);
	const auto immr = field(word, 16, 6), imms = field(word, 10, 6);
	if (operation == 3 ||
		(width == 64 ? n != 1 : n != 0 || immr >= 32 || imms >= 32))
		return false;
	const auto masks = decode_bit_masks(n, imms, immr, false, width);
	if (!masks)
		return false;
	const auto rd = field(word, 0, 5);
	const auto source = read(cpu, field(word, 5, 5), width);
	const auto bottom = shift(source, ror, immr, width) & masks->wmask;
	std::uint64_t result{};
	if (operation == 0) {
		const auto top = (source >> imms & 1U) != 0 ? ones(width) : 0U;
		result = (top & ~masks->tmask) | (bottom & masks->tmask);
	} else if (operation == 1) {
		const auto destination = read(cpu, rd, width);
		const auto moved = (destination & ~masks->wmask) | bottom;
		result = (destination & ~masks->tmask) | (moved & masks->tmask);
	} else {
		result = bottom & masks->tmask;
	}
	write(cpu, rd, result, width);
	return true;
}

bool extract(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto lsb = field(word, 10, 6);
	if (field(word, 22, 1) != (width == 64 ? 1U : 0U) || lsb >= width)
		return false;
	const auto low = read(cpu, field(word, 16, 5), width);
	const auto high = read(cpu, field(word, 5, 5), width);
	const auto result = lsb == 0 ? low :
		((low >> lsb) | (high << (width - lsb))) & ones(width);
	write(cpu, field(word, 0, 5), result, width);
	return true;
}

bool conditional_compare(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const bool subtract = field(word, 30, 1) != 0;
	const auto operand = field(word, 11, 1) != 0 ? std::uint64_t{field(word, 16, 5)} :
		read(cpu, field(word, 16, 5), width);
	cpu.nzcv = condition_holds(cpu.nzcv, field(word, 12, 4)) ?
		add_with_carry(read(cpu, field(word, 5, 5), width),
			subtract ? ~operand : operand, subtract, width).nzcv :
		field(word, 0, 4) << 28;
	return true;
}

bool conditional_select(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	auto result = read(cpu, field(word, 5, 5), width);
	if (!condition_holds(cpu.nzcv, field(word, 12, 4))) {
		result = read(cpu, field(word, 16, 5), width);
		if (field(word, 30, 1) != 0)
			result = ~result;
		if (field(word, 10, 1) != 0)
			++result;
	}
	write(cpu, field(word, 0, 5), result, width);
	return true;
}

/* SMAX, UMAX, SMIN and UMIN, selected by opc<1:0>. */
std::uint64_t minimum_maximum(std::uint64_t left, std::uint64_t right,
	unsigned operation, unsigned width)
{
	const bool is_unsigned = (operation & 1U) != 0, minimum = (operation & 2U) != 0;
	const bool less = is_unsigned ? left < right :
		sign_extend(left, width) < sign_extend(right, width);
	return less != minimum ? right : left;
}

bool minimum_maximum_immediate(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto operation = field(word, 18, 4), immediate = field(word, 10, 8);
	if (operation > 3)
		return false;
	const auto operand = (operation & 1U) != 0 ? std::uint64_t{immediate} :
		static_cast<std::uint64_t>(sign_extend(immediate, 8)) & ones(width);
	write(cpu, field(word, 0, 5), minimum_maximum(read(cpu, field(word, 5, 5), width),
		operand, operation, width), width);
	return true;
}

/* The reflected form of CRC32 and CRC32C, without pre- or post-inversion. */
std::uint32_t crc32(std::uint32_t accumulator, std::uint64_t value, unsigned bytes,
	std::uint32_t reflected_polynomial)
{
	for (unsigned byte = 0; byte < bytes; ++byte) {
		accumulator ^= static_cast<std::uint32_t>(value >> (8U * byte) & 0xffU);
		for (unsigned bit = 0; bit < 8; ++bit)
			accumulator = (accumulator >> 1) ^
				((accumulator & 1U) != 0 ? reflected_polynomial : 0U);
	}
	return accumulator;
}

bool two_source(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto opcode = field(word, 10, 6), rd = field(word, 0, 5);
	const auto left = read(cpu, field(word, 5, 5), width);
	const auto right = read(cpu, field(word, 16, 5), width);
	std::uint64_t result{};
	if (opcode == 2) {
		result = right == 0 ? 0U : left / right;
	} else if (opcode == 3) {
		const auto dividend = sign_extend(left, width), divisor = sign_extend(right, width);
		result = divisor == 0 ? 0U : divisor == -1 ? 0U - left :
			static_cast<std::uint64_t>(dividend / divisor);
	} else if (opcode >= 8 && opcode <= 11) {
		result = shift(left, opcode - 8U, static_cast<unsigned>(right % width), width);
	} else if (opcode >= 16 && opcode <= 23) {
		const auto size = field(word, 10, 2);
		if ((size == 3) != (width == 64))
			return false;
		write(cpu, rd, crc32(static_cast<std::uint32_t>(left), right, 1U << size,
			opcode >= 20 ? 0x82f6'3b78U : 0xedb8'8320U), 32);
		return true;
	} else if (opcode >= 24 && opcode <= 27) {
		result = minimum_maximum(left, right, opcode - 24U, width);
	} else {
		return false;
	}
	write(cpu, rd, result, width);
	return true;
}

std::uint64_t reverse_bytes(std::uint64_t value, unsigned container, unsigned width)
{
	std::uint64_t result{};
	for (unsigned base = 0; base < width; base += container)
		for (unsigned byte = 0; byte < container / 8U; ++byte)
			result |= (value >> (base + 8U * byte) & 0xffU) <<
				(base + container - 8U * (byte + 1U));
	return result;
}

unsigned leading_zeros(std::uint64_t value, unsigned width)
{
	return static_cast<unsigned>(std::countl_zero(value)) - (64U - width);
}

bool one_source(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto value = read(cpu, field(word, 5, 5), width);
	std::uint64_t result{};
	switch (field(word, 10, 6)) {
	case 0:
		for (unsigned bit = 0; bit < width; ++bit)
			result |= (value >> bit & 1U) << (width - 1U - bit);
		break;
	case 1: result = reverse_bytes(value, 16, width); break;
	case 2: result = reverse_bytes(value, 32, width); break;
	case 3:
		if (width != 64)
			return false;
		result = reverse_bytes(value, 64, width);
		break;
	case 4: result = leading_zeros(value, width); break;
	case 5:
		result = leading_zeros(((value >> 1) ^ value) & ones(width - 1U), width - 1U);
		break;
	case 6:
		result = value == 0 ? width : static_cast<unsigned>(std::countr_zero(value));
		break;
	case 7: result = static_cast<unsigned>(std::popcount(value)); break;
	case 8: result = sign_extend(value, width) < 0 ? 0U - value : value; break;
	default: return false;
	}
	write(cpu, field(word, 0, 5), result, width);
	return true;
}

bool multiply_add(CpuState &cpu, std::uint32_t word)
{
	const auto width = operand_width(word);
	const auto operation = field(word, 21, 3);
	const auto rn = field(word, 5, 5), rm = field(word, 16, 5);
	std::uint64_t product{};
	if (operation == 0)
		product = read(cpu, rn, width) * read(cpu, rm, width);
	else if (width == 64 && operation == 1)
		product = static_cast<std::uint64_t>(sign_extend(read(cpu, rn, 32), 32) *
			sign_extend(read(cpu, rm, 32), 32));
	else if (width == 64 && operation == 5)
		product = read(cpu, rn, 32) * read(cpu, rm, 32);
	else
		return false;
	const auto addend = read(cpu, field(word, 10, 5), width);
	write(cpu, field(word, 0, 5),
		field(word, 15, 1) != 0 ? addend - product : addend + product, width);
	return true;
}

bool vector_orr(CpuState &cpu, std::uint32_t word)
{
	const auto bytes = field(word, 30, 1) != 0 ? 16U : 8U;
	const auto &left = cpu.vectors[field(word, 5, 5)];
	const auto &right = cpu.vectors[field(word, 16, 5)];
	VectorRegister result{};
	for (unsigned byte = 0; byte < bytes; ++byte)
		result.bytes[byte] = left.bytes[byte] | right.bytes[byte];
	cpu.vectors[field(word, 0, 5)] = result;
	return true;
}

bool vector_add(CpuState &cpu, std::uint32_t word)
{
	const auto size = field(word, 22, 2), full = field(word, 30, 1);
	if (size == 3 && full == 0)
		return false;
	const auto lane_bytes = 1U << size, bytes = full != 0 ? 16U : 8U;
	const auto &left = cpu.vectors[field(word, 5, 5)];
	const auto &right = cpu.vectors[field(word, 16, 5)];
	VectorRegister result{};
	for (unsigned index = 0; index < bytes / lane_bytes; ++index)
		set_lane(result, index, lane_bytes,
			lane(left, index, lane_bytes) + lane(right, index, lane_bytes));
	cpu.vectors[field(word, 0, 5)] = result;
	return true;
}

/* MOVI Dd, #imm and MOVI Vd.2D, #imm: each immediate bit selects a byte. */
bool vector_move_immediate(CpuState &cpu, std::uint32_t word)
{
	const auto immediate = (field(word, 16, 3) << 5) | field(word, 5, 5);
	std::uint64_t expanded{};
	for (unsigned bit = 0; bit < 8; ++bit)
		if ((immediate >> bit & 1U) != 0)
			expanded |= std::uint64_t{0xff} << (8U * bit);
	VectorRegister result{};
	set_lane(result, 0, 8, expanded);
	if (field(word, 30, 1) != 0)
		set_lane(result, 1, 8, expanded);
	cpu.vectors[field(word, 0, 5)] = result;
	return true;
}

bool vector_unsigned_move(CpuState &cpu, std::uint32_t word)
{
	const auto imm5 = field(word, 16, 5);
	const auto size = static_cast<unsigned>(std::countr_zero(imm5));
	if (size > 3 || (size == 3) != (field(word, 30, 1) != 0))
		return false;
	const auto bytes = 1U << size;
	write(cpu, field(word, 0, 5),
		lane(cpu.vectors[field(word, 5, 5)], imm5 >> (size + 1U), bytes), 64);
	return true;
}

struct Family final {
	std::uint32_t mask;
	std::uint32_t value;
	/* Returns false, before any state change, for a reserved encoding. */
	bool (*execute)(CpuState &, std::uint32_t);
};

constexpr std::array families{
	Family{0x1f80'0000U, 0x1100'0000U, add_subtract_immediate},
	Family{0x7fc0'0000U, 0x11c0'0000U, minimum_maximum_immediate},
	Family{0x1f80'0000U, 0x1200'0000U, logical_immediate},
	Family{0x1f80'0000U, 0x1280'0000U, move_wide},
	Family{0x1f80'0000U, 0x1300'0000U, bitfield},
	Family{0x7fa0'0000U, 0x1380'0000U, extract},
	Family{0x1f00'0000U, 0x0a00'0000U, logical_shifted},
	Family{0x1f20'0000U, 0x0b00'0000U, add_subtract_shifted},
	Family{0x1fe0'0000U, 0x0b20'0000U, add_subtract_extended},
	Family{0x1fe0'fc00U, 0x1a00'0000U, add_subtract_carry},
	Family{0x3fe0'0410U, 0x3a40'0000U, conditional_compare},
	Family{0x3fe0'0800U, 0x1a80'0000U, conditional_select},
	Family{0x7fe0'0000U, 0x1ac0'0000U, two_source},
	Family{0x7fff'0000U, 0x5ac0'0000U, one_source},
	Family{0x7f00'0000U, 0x1b00'0000U, multiply_add},
	Family{0xbfe0'fc00U, 0x0ea0'1c00U, vector_orr},
	Family{0xbf20'fc00U, 0x0e20'8400U, vector_add},
	Family{0xbff8'fc00U, 0x2f00'e400U, vector_move_immediate},
	Family{0xbfe0'fc00U, 0x0e00'3c00U, vector_unsigned_move},
};

} // namespace

Step execute(CpuState &cpu, std::uint32_t instruction)
{
	if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
		cpu.pc += 4;
		return Step::system_call;
	}
	if (instruction == 0xd503'201fU) {
		cpu.pc += 4;
		return Step::continued;
	}
	if ((instruction & 0xff00'0010U) == 0x5400'0000U) {
		const auto offset = sign_extend(field(instruction, 5, 19), 19) * 4;
		cpu.pc += condition_holds(cpu.nzcv, field(instruction, 0, 4)) ?
			static_cast<std::uint64_t>(offset) : 4U;
		return Step::continued;
	}
	for (const auto &family : families)
		if ((instruction & family.mask) == family.value) {
			if (!family.execute(cpu, instruction))
				return Step::unsupported;
			cpu.pc += 4;
			return Step::continued;
		}
	return Step::unsupported;
}

} // namespace arm64_gadget_dependency_reference
