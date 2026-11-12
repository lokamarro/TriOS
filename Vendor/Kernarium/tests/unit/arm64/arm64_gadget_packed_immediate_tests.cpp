// SPDX-License-Identifier: GPL-2.0-or-later
/* The immediates the gadget engine packs into the operand word that already
 * carries a register: a constant store shares one word between the
 * destination's offset and a value of up to 48 bits (or of ones above bit 47).
 * Each form runs against the architectural definition for every width, shift,
 * destination and boundary value, alone and with an adjacent consumer that
 * takes the stored value from the forwarded result. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace g = uml::cpu::arm64::gadgets;
namespace a = uml::cpu::arm64;

extern "C" {
void uml_arm64_gadget_svc();
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_packed_zero();
void uml_arm64_gadget_move_wide_packed_ones();
}

namespace {
using uml::emulator::tests::address;
using uml::testing::require;

constexpr std::uint64_t default_pc = 0x4000;
constexpr std::uint32_t svc = 0xd400'0001U;

std::uint64_t next_random(std::uint64_t &seed)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

a::CpuState random_state(std::uint64_t &seed,
	std::uint64_t pc = default_pc)
{
	a::CpuState cpu{};
	for (auto &value : cpu.registers)
		value = next_random(seed);
	cpu.pc = pc;
	cpu.sp = next_random(seed) & ~std::uint64_t{15};
	cpu.nzcv = static_cast<std::uint32_t>(next_random(seed)) & 0xf000'0000U;
	return cpu;
}

/* Runs the instructions and the SVC that ends them on both forwarding
 * settings and returns the state each leaves; they must agree. */
a::CpuState run(std::vector<std::uint32_t> code, const a::CpuState &initial,
	std::uint64_t pc = default_pc)
{
	code.push_back(svc);
	auto tlb = std::make_unique<uml::memory::Tlb>();
	a::CpuState result[2]{initial, initial};
	for (unsigned forwarding = 0; forwarding < 2; ++forwarding) {
		const auto built = g::Builder{}.build(pc, code, {},
			std::pmr::get_default_resource(), {},
			{.scalar_forwarding = forwarding != 0});
		require(built.complete);
		std::size_t executed{};
		require(g::execute(built.program, result[forwarding], *tlb,
			executed) == uml::cpu::EngineEvent::system_call);
		require(executed == code.size());
	}
	require(result[0].registers == result[1].registers);
	require(result[0].nzcv == result[1].nzcv && result[0].sp == result[1].sp);
	return result[1];
}

std::uint32_t move_wide(bool wide, unsigned opc, unsigned halfword,
	unsigned immediate, unsigned destination)
{
	return (wide ? 0x8000'0000U : 0U) | 0x1280'0000U | (opc << 29) |
		(halfword << 21) | (immediate << 5) | destination;
}

/* MOVZ and MOVN from the architecture's definition: the immediate at the
 * halfword, inverted for MOVN, truncated to 32 bits for a W destination. */
std::uint64_t expected_move(bool wide, unsigned opc, unsigned halfword,
	unsigned immediate)
{
	auto value = static_cast<std::uint64_t>(immediate) << (halfword * 16);
	if (opc == 0)
		value = ~value;
	return wide ? value : value & 0xffff'ffffU;
}

void move_wide_forms()
{
	constexpr std::array immediates{0U, 1U, 0x1234U, 0x7fffU, 0x8000U,
		0xfffeU, 0xffffU};
	constexpr std::array destinations{0U, 1U, 5U, 17U, 29U, 30U, 31U};
	std::uint64_t seed = 0x2f9a'51c4'80e7'3b65ULL;
	std::size_t cases{}, packed{}, wide_only{};
	for (const bool wide : {false, true})
	for (const unsigned opc : {0U, 2U})
	for (unsigned halfword = 0; halfword < (wide ? 4U : 2U); ++halfword)
	for (const auto immediate : immediates)
	for (const auto destination : destinations) {
		const auto word = move_wide(wide, opc, halfword, immediate,
			destination);
		const auto value = expected_move(wide, opc, halfword, immediate);
		const auto initial = random_state(seed);

		auto expected = initial;
		if (destination != 31U)
			expected.registers[destination] = value;
		const auto alone = run({word}, initial);
		require(alone.registers == expected.registers);
		require(alone.nzcv == expected.nzcv && alone.sp == expected.sp);

		/* The store's value reaches the next instruction either through
		 * the forwarded result or through the register. */
		if (destination != 31U) {
			const unsigned result = destination == 20U ? 21U : 20U;
			auto consumed = expected;
			consumed.registers[result] = (wide ? value :
				value & 0xffff'ffffU) + 1U;
			if (!wide)
				consumed.registers[result] &= 0xffff'ffffU;
			const auto sum = 0x1100'0400U | (wide ? 0x8000'0000U : 0U) |
				(destination << 5) | result;
			const auto after = run({word, sum}, initial);
			require(after.registers == consumed.registers);
		}

		/* The stream: one operand word when the value fits 48 bits or is
		 * ones above them, the offset and the value in two otherwise. */
		const auto built = g::Builder{}.build(default_pc,
			std::array{word, svc});
		const auto words = built.program.words();
		if (destination == 31U)
			continue;
		const auto offset = std::uint64_t{destination} * 8U;
		constexpr std::uint64_t low_48 = (std::uint64_t{1} << 48) - 1;
		if ((value & ~low_48) == 0) {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_packed_zero));
			require(words[1] == (offset << 48 | value));
			require(words[2] == address(uml_arm64_gadget_svc));
			++packed;
		} else if ((value & ~low_48) == ~low_48) {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_packed_ones));
			require(words[1] == (offset << 48 | (value & low_48)));
			require(words[2] == address(uml_arm64_gadget_svc));
			++packed;
		} else {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_constant));
			require(words[1] == offset && words[2] == value);
			require(words[3] == address(uml_arm64_gadget_svc));
			++wide_only;
		}
		++cases;
	}
	require(packed > 0 && wide_only > 0);
	std::printf("move wide: %zu cases, %zu in one word, %zu in two\n",
		cases, packed, wide_only);
}

/* The packed word for a value, or none when it needs two. */
enum class Shape { zero, ones, wide };

Shape shape_of(std::uint64_t value)
{
	constexpr std::uint64_t low_48 = (std::uint64_t{1} << 48) - 1;
	return (value & ~low_48) == 0 ? Shape::zero :
		(value & ~low_48) == ~low_48 ? Shape::ones : Shape::wide;
}

void require_constant_store(std::span<const std::uintptr_t> words,
	unsigned destination, std::uint64_t value)
{
	constexpr std::uint64_t low_48 = (std::uint64_t{1} << 48) - 1;
	const auto offset = std::uint64_t{destination} * 8U;
	switch (shape_of(value)) {
	case Shape::zero:
		require(words[0] == address(uml_arm64_gadget_move_wide_packed_zero));
		require(words[1] == (offset << 48 | value));
		require(words[2] == address(uml_arm64_gadget_svc));
		break;
	case Shape::ones:
		require(words[0] == address(uml_arm64_gadget_move_wide_packed_ones));
		require(words[1] == (offset << 48 | (value & low_48)));
		require(words[2] == address(uml_arm64_gadget_svc));
		break;
	case Shape::wide:
		require(words[0] == address(uml_arm64_gadget_move_wide_constant));
		require(words[1] == offset && words[2] == value);
		require(words[3] == address(uml_arm64_gadget_svc));
		break;
	}
}

std::uint32_t pc_relative(bool page, std::int64_t immediate,
	unsigned destination)
{
	const auto field = static_cast<std::uint32_t>(immediate) & 0x1fffffU;
	return (page ? 0x9000'0000U : 0x1000'0000U) | ((field & 3U) << 29) |
		((field >> 2) << 5) | destination;
}

std::uint64_t expected_pc_relative(bool page, std::uint64_t at,
	std::int64_t immediate)
{
	return page ? (at & ~std::uint64_t{0xfff}) +
			static_cast<std::uint64_t>(immediate * 4096) :
		at + static_cast<std::uint64_t>(immediate);
}

/* ADR and ADRP, and the ADRP + ADD idiom folded into one store, at program
 * counters on both sides of the 48-bit boundary and the top of the address
 * space, where the constant needs a word of its own or is ones above bit 47. */
void pc_relative_forms()
{
	constexpr std::array counters{
		std::uint64_t{0x4000}, std::uint64_t{0x7fff'ffff'f000},
		std::uint64_t{0xffff'ffff'fffc}, std::uint64_t{0x0001'0000'0000'0000},
		std::uint64_t{0xffff'0000'0000'1000}, std::uint64_t{0xffff'ffff'ffff'f000},
		std::uint64_t{0x0000'ffff'ffff'e000}};
	constexpr std::array<std::int64_t, 8> immediates{
		0, 1, -1, 12345, -12345, 0xfffff, -0x100000, 0x80000};
	constexpr std::array destinations{0U, 3U, 17U, 30U, 31U};
	std::uint64_t seed = 0x6c3e'a0d2'91b8'47f5ULL;
	std::size_t cases{}, packed{}, wide{}, folded{};
	for (const auto at : counters)
	for (const bool page : {false, true})
	for (const auto immediate : immediates)
	for (const auto destination : destinations) {
		const auto word = pc_relative(page, immediate, destination);
		const auto value = expected_pc_relative(page, at, immediate);
		const auto initial = random_state(seed, at);
		auto expected = initial;
		if (destination != 31U)
			expected.registers[destination] = value;
		const auto alone = run({word}, initial, at);
		require(alone.registers == expected.registers);

		const auto built = g::Builder{}.build(at, std::array{word, svc});
		if (destination != 31U) {
			require_constant_store(built.program.words(), destination,
				value);
			shape_of(value) == Shape::wide ? ++wide : ++packed;
		}

		if (destination == 31U || destination == 0U)
			continue;
		/* The ADD that rewrites the register ADRP wrote folds into the same
		 * store; any other ADD reads it from the forwarded result. */
		for (const unsigned low : {0U, 1U, 0xfffU}) {
			const auto add = 0x9100'0000U | (low << 10) |
				(destination << 5) | destination;
			const auto pair_at = at;
			auto both = expected;
			both.registers[destination] = value + low;
			const auto after = run({word, add}, initial, pair_at);
			require(after.registers == both.registers);
			if (page) {
				const auto pair = g::Builder{}.build(at,
					std::array{word, add, svc});
				require(pair.complete);
				require_constant_store(pair.program.words(), destination,
					value + low);
				++folded;
			}
		}
		const unsigned other = destination == 20U ? 21U : 20U;
		const auto copy = 0x9100'0400U | (destination << 5) | other;
		auto consumed = expected;
		consumed.registers[other] = value + 1U;
		require(run({word, copy}, initial, at).registers == consumed.registers);
		++cases;
	}
	require(packed > 0 && wide > 0 && folded > 0);
	std::printf("pc relative: %zu cases, %zu stores in one word, %zu in two, "
		"%zu folded ADRP+ADD\n", cases, packed, wide, folded);
}

/* DecodeBitMasks for a logical immediate, written from the Arm pseudocode. */
bool logical_bitmask(bool wide, unsigned n, unsigned immr, unsigned imms,
	std::uint64_t &result)
{
	if (!wide && n != 0)
		return false;
	const unsigned combined = (n << 6) | (~imms & 0x3fU);
	if (combined == 0)
		return false;
	unsigned length = 0;
	while ((combined >> (length + 1)) != 0)
		++length;
	if (length < 1)
		return false;
	const unsigned size = 1U << length;
	if (size > (wide ? 64U : 32U))
		return false;
	const unsigned levels = size - 1;
	if ((imms & levels) == levels)
		return false;
	const unsigned ones = (imms & levels) + 1;
	const unsigned rotate = immr & levels;
	std::uint64_t element = ones == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << ones) - 1;
	if (rotate != 0) {
		const std::uint64_t mask = size == 64 ? ~std::uint64_t{0} :
			(std::uint64_t{1} << size) - 1;
		element = ((element >> rotate) | (element << (size - rotate))) & mask;
	}
	result = 0;
	for (unsigned at = 0; at < (wide ? 64U : 32U); at += size)
		result |= element << at;
	return true;
}

std::uint32_t logical_immediate(bool wide, unsigned opc, unsigned n,
	unsigned immr, unsigned imms, unsigned source, unsigned destination)
{
	return (wide ? 0x8000'0000U : 0U) | (opc << 29) | 0x1200'0000U |
		(n << 22) | (immr << 16) | (imms << 10) | (source << 5) | destination;
}

std::uint32_t add_immediate(bool wide, unsigned source, unsigned destination,
	unsigned immediate)
{
	return (wide ? 0x9100'0000U : 0x1100'0000U) | (immediate << 10) |
		(source << 5) | destination;
}

/* AND, ORR, EOR and ANDS (immediate) against the definition: Rn reads zero for
 * register 31, Rd is SP for the three that do not set flags and discards for
 * ANDS. Alone, in place (Rd == Rn), and beside ADDs that produce its source or
 * consume its result. The stream: a 32-bit immediate shares the register word,
 * a 64-bit one has its own, and an in-place form has the immediate alone. */
void logical_immediate_forms()
{
	struct Registers final { unsigned source, destination; };
	constexpr std::array<Registers, 6> shapes{{
		{5, 5}, {3, 7}, {31, 3}, {4, 31}, {29, 29}, {30, 2}}};
	std::uint64_t seed = 0x91e4'20b7'c85d'3a06ULL;
	std::size_t cases{}, packed{}, in_place{}, wide_words{};
	unsigned encoding_index = 0;
	for (const bool wide : {false, true})
	for (unsigned n = 0; n < (wide ? 2U : 1U); ++n)
	for (unsigned immr = 0; immr < (wide ? 64U : 32U); ++immr)
	for (unsigned imms = 0; imms < (wide ? 64U : 32U); ++imms) {
		std::uint64_t immediate;
		if (!logical_bitmask(wide, n, immr, imms, immediate))
			continue;
		/* Every W encoding, one in five of the X ones. */
		if (wide && (++encoding_index % 5U) != 0)
			continue;
		const std::uint64_t mask = wide ? ~std::uint64_t{0} : 0xffff'ffffU;
		for (unsigned opc = 0; opc < 4; ++opc)
		for (const auto shape : shapes) {
			const auto word = logical_immediate(wide, opc, n, immr, imms,
				shape.source, shape.destination);
			const auto initial = random_state(seed);
			const std::uint64_t operand = shape.source == 31U ? 0 :
				initial.registers[shape.source] & mask;
			std::uint64_t result = opc == 0 || opc == 3 ? operand & immediate :
				opc == 1 ? operand | immediate : operand ^ immediate;
			result &= mask;
			auto expected = initial;
			if (opc == 3) {
				expected.nzcv = (initial.nzcv & 0x0fff'ffffU) |
					((result >> (wide ? 63 : 31)) << 31) |
					(result == 0 ? 1U << 30 : 0U);
			}
			if (shape.destination == 31U) {
				if (opc != 3)
					expected.sp = result;
			} else {
				expected.registers[shape.destination] = result;
			}
			const auto alone = run({word}, initial);
			require(alone.registers == expected.registers);
			require(alone.nzcv == expected.nzcv && alone.sp == expected.sp);

			/* The stream. */
			const auto built = g::Builder{}.build(default_pc,
				std::array{word, svc});
			const auto words = built.program.words();
			const std::uintptr_t svc_entry = address(uml_arm64_gadget_svc);
			const bool same = shape.source == shape.destination &&
				shape.destination != 31U;
			if (same) {
				require(words[1] == immediate && words[2] == svc_entry);
				++in_place;
			} else if (wide) {
				require(words[1] == (shape.destination |
					std::uint64_t{shape.source} << 8) &&
					words[2] == immediate && words[3] == svc_entry);
				++wide_words;
			} else {
				require(words[1] == (shape.destination |
					std::uint64_t{shape.source} << 8 | immediate << 32) &&
					words[2] == svc_entry);
				++packed;
			}

			/* Its result consumed by the next ADD, forwarded or not. */
			if (shape.destination != 31U) {
				const unsigned other = shape.destination == 20U ? 21U : 20U;
				auto consumed = expected;
				const auto base = result;
				consumed.registers[other] = wide ? base + 3U :
					(base + 3U) & 0xffff'ffffU;
				require(run({word, add_immediate(wide, shape.destination,
					other, 3)}, initial).registers == consumed.registers);
			}
			/* Its source produced by the ADD before it. */
			if (shape.source != 31U) {
				const auto produce = add_immediate(wide, 6, shape.source, 9);
				auto before = initial;
				before.registers[shape.source] = wide ?
					initial.registers[6] + 9U :
					(initial.registers[6] + 9U) & 0xffff'ffffU;
				const auto operand2 = before.registers[shape.source] & mask;
				std::uint64_t second = opc == 0 || opc == 3 ? operand2 & immediate :
					opc == 1 ? operand2 | immediate : operand2 ^ immediate;
				second &= mask;
				auto after = before;
				if (opc == 3) {
					after.nzcv = (initial.nzcv & 0x0fff'ffffU) |
						((second >> (wide ? 63 : 31)) << 31) |
						(second == 0 ? 1U << 30 : 0U);
				}
				if (shape.destination == 31U) {
					if (opc != 3)
						after.sp = second;
				} else {
					after.registers[shape.destination] = second;
				}
				const auto fed = run({produce, word}, initial);
				require(fed.registers == after.registers);
				require(fed.nzcv == after.nzcv && fed.sp == after.sp);
			}
			++cases;
		}
	}
	require(packed > 0 && in_place > 0 && wide_words > 0);
	std::printf("logical immediate: %zu cases, %zu W in the register word, "
		"%zu in place, %zu X with a word of their own\n",
		cases, packed, in_place, wide_words);
}

} // namespace

int main()
{
	move_wide_forms();
	pc_relative_forms();
	logical_immediate_forms();
	std::puts("gadget packed immediate tests passed");
}
