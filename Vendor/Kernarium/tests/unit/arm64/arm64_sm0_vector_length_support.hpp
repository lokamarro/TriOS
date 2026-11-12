// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * The SM = 0 vector-length family (cpu/arm64/detail/sm0_vector_length.hpp)
 * as the tests state it: the words of every form and a model of what each
 * does to the general registers at a vector length of 16 bytes, written
 * from DecodePredCount() and the instructions' pseudocode and independent
 * of the library under test; and the spill words (LDR and STR of Z and P),
 * with a model of what each does to V and to memory, for a CPU that has no
 * predicate state.
 */
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace uml::emulator::tests::sm0_vector_length {

inline constexpr unsigned vector_bytes = 16;

/* CNT[BHWD] Xd, pattern, MUL #m (size 0-3 for B, H, W, D). */
[[nodiscard]] constexpr std::uint32_t cnt(unsigned size, unsigned pattern,
	unsigned multiplier, unsigned rd) noexcept
{
	return 0x0420'e000U | (size << 22) | ((multiplier - 1U) << 16) |
		(pattern << 5) | rd;
}

/* INC[BHWD] and DEC[BHWD] Xdn, pattern, MUL #m. */
[[nodiscard]] constexpr std::uint32_t inc_dec(bool decrement, unsigned size,
	unsigned pattern, unsigned multiplier, unsigned rdn) noexcept
{
	return 0x0430'e000U | (decrement ? 0x400U : 0U) | (size << 22) |
		((multiplier - 1U) << 16) | (pattern << 5) | rdn;
}

[[nodiscard]] constexpr std::uint32_t rdvl(int immediate, unsigned rd) noexcept
{
	return 0x04bf'5000U | ((static_cast<unsigned>(immediate) & 63U) << 5) |
		rd;
}

/* ADDVL (predicate false) and ADDPL (true) Xd|SP, Xn|SP, #imm. */
[[nodiscard]] constexpr std::uint32_t add_length(bool predicate,
	unsigned rd, unsigned rn, int immediate) noexcept
{
	return (predicate ? 0x0460'5000U : 0x0420'5000U) | (rn << 16) |
		((static_cast<unsigned>(immediate) & 63U) << 5) | rd;
}

/* DecodePredCount(pattern, elements): the element count a predicate
 * constraint names; the reserved patterns name none. */
[[nodiscard]] constexpr unsigned pattern_count(unsigned pattern,
	unsigned elements) noexcept
{
	unsigned count = 0;
	if (pattern == 0U)
		count = std::bit_floor(elements);
	else if (pattern <= 8U)
		count = pattern;
	else if (pattern >= 9U && pattern <= 13U)
		count = 16U << (pattern - 9U);
	else if (pattern == 29U)
		count = elements - elements % 4U;
	else if (pattern == 30U)
		count = elements - elements % 3U;
	else if (pattern == 31U)
		count = elements;
	return count <= elements ? count : 0U;
}

/* The general registers a family word reads and writes. */
struct Registers final {
	std::array<std::uint64_t, 31> x{};
	std::uint64_t sp{};
};

/* What the word does to `registers`; false for a word outside the family. */
[[nodiscard]] constexpr bool model(std::uint32_t word,
	Registers &registers) noexcept
{
	const unsigned rd = word & 31U;
	const unsigned rn = (word >> 16) & 31U;
	const unsigned size = (word >> 22) & 3U;
	const unsigned elements = vector_bytes >> size;
	const unsigned multiplier = ((word >> 16) & 15U) + 1U;
	const unsigned pattern = (word >> 5) & 31U;
	const std::int64_t immediate = ((word >> 5) & 63U) >= 32U ?
		static_cast<std::int64_t>((word >> 5) & 63U) - 64 :
		static_cast<std::int64_t>((word >> 5) & 63U);
	const auto write = [&](std::uint64_t value) {
		if (rd != 31U)
			registers.x[rd] = value;
	};
	if ((word & 0xff30'fc00U) == 0x0420'e000U) { // CNT
		write(static_cast<std::uint64_t>(pattern_count(pattern, elements)) *
			multiplier);
		return true;
	}
	if ((word & 0xff30'fc00U) == 0x0430'e000U ||
	    (word & 0xff30'fc00U) == 0x0430'e400U) { // INC, DEC
		const std::uint64_t count =
			static_cast<std::uint64_t>(pattern_count(pattern, elements)) *
			multiplier;
		if (rd != 31U)
			registers.x[rd] += (word & 0x400U) != 0U ? 0U - count : count;
		return true;
	}
	if ((word & 0xffff'f800U) == 0x04bf'5000U) { // RDVL
		write(static_cast<std::uint64_t>(immediate *
			static_cast<std::int64_t>(vector_bytes)));
		return true;
	}
	if ((word & 0xffa0'f800U) == 0x0420'5000U) { // ADDVL, ADDPL
		const std::int64_t scale = (word & 0x0040'0000U) != 0U ?
			vector_bytes / 8 : vector_bytes;
		const std::uint64_t base = rn == 31U ? registers.sp :
			registers.x[rn];
		const std::uint64_t result = base +
			static_cast<std::uint64_t>(immediate * scale);
		if (rd == 31U)
			registers.sp = result;
		else
			registers.x[rd] = result;
		return true;
	}
	return false;
}

/* Family words to run: every size, every predicate pattern and several
 * multipliers of CNT, INC and DEC, each in destination registers that cover
 * XZR and the highest register; RDVL for every immediate; ADDVL and ADDPL
 * for every immediate, over distinct, equal and SP registers. */
[[nodiscard]] inline std::vector<std::uint32_t> words()
{
	std::vector<std::uint32_t> result;
	for (unsigned size = 0; size < 4U; ++size)
		for (unsigned pattern = 0; pattern < 32U; ++pattern)
			for (const unsigned multiplier : {1U, 2U, 7U, 16U})
				for (const unsigned rd : {0U, 5U, 30U, 31U}) {
					result.push_back(cnt(size, pattern, multiplier, rd));
					result.push_back(
						inc_dec(false, size, pattern, multiplier, rd));
					result.push_back(
						inc_dec(true, size, pattern, multiplier, rd));
				}
	for (int immediate = -32; immediate < 32; ++immediate) {
		for (const unsigned rd : {0U, 5U, 30U, 31U})
			result.push_back(rdvl(immediate, rd));
		for (const bool predicate : {false, true})
			for (const auto [rd, rn] :
			     {std::pair{0U, 1U}, std::pair{3U, 3U}, std::pair{31U, 31U},
			      std::pair{2U, 31U}, std::pair{31U, 4U},
			      std::pair{30U, 30U}})
				result.push_back(add_length(predicate, rd, rn, immediate));
	}
	return result;
}

/* LDR and STR (vector) Zt, [Xn|SP, #imm, MUL VL] and LDR and STR
 * (predicate) Pt, [Xn|SP, #imm, MUL VL]. */
[[nodiscard]] constexpr std::uint32_t spill_word(bool store, bool predicate,
	unsigned transfer, unsigned base, int immediate) noexcept
{
	const auto imm9 = static_cast<unsigned>(immediate) & 511U;
	return (store ? 0xe580'0000U : 0x8580'0000U) | (predicate ? 0U : 0x4000U) |
		((imm9 >> 3) << 16) | ((imm9 & 7U) << 10) | (base << 5) | transfer;
}

/* A spill word's fields, as the test chose them. */
struct Spill final {
	bool store{};
	bool predicate{};
	unsigned transfer{};
	unsigned base{};
	int immediate{};

	[[nodiscard]] constexpr std::uint32_t word() const noexcept
	{
		return spill_word(store, predicate, transfer, base, immediate);
	}
	[[nodiscard]] constexpr unsigned bytes() const noexcept
	{
		return predicate ? vector_bytes / 8U : vector_bytes;
	}
	/* MUL VL scales by the register's size at the constant vector length. */
	[[nodiscard]] constexpr std::int64_t offset() const noexcept
	{
		return static_cast<std::int64_t>(immediate) * bytes();
	}
};

/* Spill words to run: each of the four forms over immediates at the ends of
 * the signed 9-bit range, around the LDUR range of the access the engines
 * make (-16 vectors, -128 predicates), and the offsets real code uses, over
 * transfer registers including the highest, and bases including x0, the
 * frame pointer and SP. */
[[nodiscard]] inline std::vector<Spill> spills()
{
	std::vector<Spill> result;
	for (const bool predicate : {false, true})
		for (const bool store : {false, true})
			for (const int immediate :
			     {-256, -255, -129, -128, -127, -19, -17, -16, -15, -2, -1,
			      0, 1, 2, 7, 8, 15, 16, 17, 31, 127, 128, 255})
				for (const unsigned transfer : {0U, 5U, 15U})
					for (const unsigned base : {0U, 29U, 31U})
						result.push_back(
							{store, predicate, transfer, base, immediate});
	/* The highest vector register. */
	for (const bool store : {false, true})
		for (const int immediate : {-19, 0, 17})
			result.push_back({store, false, 31U, 31U, immediate});
	return result;
}

/* The V registers and a window of guest memory a spill word acts on. */
struct SpillImage final {
	std::array<std::array<std::uint8_t, 16>, 32> v{};
	std::vector<std::uint8_t> memory;
};

/* What a spill word does at `index` of the memory window, from the
 * pseudocode with a vector length of 16 bytes: the vector register is the V
 * register; the CPU has no predicate state, so a predicate store writes
 * zeros and a predicate load only reads. */
inline void apply_spill(const Spill &spill, std::size_t index,
	SpillImage &image) noexcept
{
	if (spill.predicate) {
		if (spill.store)
			for (unsigned byte = 0; byte < spill.bytes(); ++byte)
				image.memory[index + byte] = 0;
		return;
	}
	for (unsigned byte = 0; byte < spill.bytes(); ++byte) {
		if (spill.store)
			image.memory[index + byte] = image.v[spill.transfer][byte];
		else
			image.v[spill.transfer][byte] = image.memory[index + byte];
	}
}

} // namespace uml::emulator::tests::sm0_vector_length
