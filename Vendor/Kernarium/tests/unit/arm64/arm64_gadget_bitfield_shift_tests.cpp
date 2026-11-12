// SPDX-License-Identifier: GPL-2.0-or-later
/* SBFM and UBFM, every encoding of both widths, run as the shift gadgets of
 * engine/bitfield_shift.inc and agree with the test-local reference: alone,
 * between an adjacent producer and consumer that forward through them, and
 * with the discarded and zero registers. The entry each shape selects, the
 * words it costs and its place in a streaming program are checked too. */
#include "arm64_gadget_dependency_reference.hpp"
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/gadget_symbols.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace g = uml::cpu::arm64::gadgets;
namespace a = uml::cpu::arm64;
namespace reference = arm64_gadget_dependency_reference;
namespace selection = g::selection;

void run_gadget_bitfield_shift_tests();

namespace {
using uml::emulator::tests::address;
using uml::testing::require;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint64_t base = 0x4000;
constexpr std::uint32_t add_x3_x1_x2 = 0x8b02'0023U;
constexpr std::uint32_t add_x6_x4_x5 = 0x8b05'0086U;

/* SBFM (opc 0) or UBFM (opc 2). */
[[nodiscard]] constexpr std::uint32_t encode(unsigned sf, unsigned opc,
	unsigned immr, unsigned imms, unsigned rn, unsigned rd) noexcept
{
	return (sf << 31) | (opc << 29) | 0x1300'0000U | (sf << 22) |
		(immr << 16) | (imms << 10) | (rn << 5) | rd;
}

/* The entry the shift amounts select, from the aliases' own conditions: LSL
 * (the field reaches the top, or the whole register), LSR and ASR (the field
 * starts at the top), and the two general forms. */
[[nodiscard]] std::uintptr_t expected_entry(unsigned sf, bool is_signed,
	unsigned immr, unsigned imms) noexcept
{
	const unsigned size = sf != 0U ? 64U : 32U;
	if (immr == (imms + 1U) % size)
		return address(sf != 0U ?
			selection::uml_arm64_gadget_bitfield_shift_x_lsl :
			selection::uml_arm64_gadget_bitfield_shift_w_lsl);
	if (imms == size - 1U) {
		if (sf != 0U)
			return address(is_signed ?
				selection::uml_arm64_gadget_bitfield_shift_x_asr :
				selection::uml_arm64_gadget_bitfield_shift_x_lsr);
		return address(is_signed ?
			selection::uml_arm64_gadget_bitfield_shift_w_asr :
			selection::uml_arm64_gadget_bitfield_shift_w_lsr);
	}
	if (sf != 0U)
		return address(is_signed ?
			selection::uml_arm64_gadget_bitfield_shift_x_sbfm :
			selection::uml_arm64_gadget_bitfield_shift_x_ubfm);
	return address(is_signed ?
		selection::uml_arm64_gadget_bitfield_shift_w_sbfm :
		selection::uml_arm64_gadget_bitfield_shift_w_ubfm);
}

[[nodiscard]] std::uint64_t next(std::uint64_t &seed) noexcept
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

[[nodiscard]] a::CpuState random_state(std::uint64_t &seed)
{
	a::CpuState cpu{};
	for (auto &value : cpu.registers)
		value = next(seed);
	cpu.pc = base;
	cpu.sp = next(seed) & ~std::uint64_t{15};
	cpu.nzcv = static_cast<std::uint32_t>(next(seed)) & 0xf000'0000U;
	return cpu;
}

void run_reference(a::CpuState &cpu, std::span<const std::uint32_t> code)
{
	for (const auto word : code)
		require(reference::execute(cpu, word) == (word == system_call ?
			reference::Step::system_call : reference::Step::continued));
}

void same_state(const a::CpuState &actual, const a::CpuState &expected)
{
	require(actual.registers == expected.registers);
	require(actual.nzcv == expected.nzcv && actual.sp == expected.sp);
	require(actual.pc == expected.pc);
}

[[nodiscard]] std::size_t system_call_words()
{
	static const std::size_t words = [] {
		const std::array code{system_call};
		return g::Builder{}.build(base, code).program.words().size();
	}();
	return words;
}

/* One instruction alone: its entry, its two words and its result. */
void single(unsigned sf, unsigned opc, unsigned immr, unsigned imms,
	unsigned rn, unsigned rd, std::uint64_t &seed, std::size_t &cases)
{
	const std::array code{encode(sf, opc, immr, imms, rn, rd), system_call};
	const auto built = g::Builder{}.build(base, code);
	require(built.complete);
	const auto words = built.program.words();
	if (rd == 31U) {
		require(words.size() == 1U + system_call_words());
		require(words[0] == address(selection::uml_arm64_gadget_nop));
	} else {
		require(words.size() == 2U + system_call_words());
		require(words[0] == expected_entry(sf, opc == 0U, immr, imms));
	}
	auto tlb = std::make_unique<uml::memory::Tlb>();
	for (unsigned trial = 0; trial < 3; ++trial) {
		auto expected = random_state(seed);
		if (trial == 1)
			expected.registers[rn == 31U ? 0 : rn] = ~std::uint64_t{0};
		if (trial == 2)
			expected.registers[rn == 31U ? 0 : rn] = 0x8000'0000'8000'0000ULL;
		auto actual = expected;
		run_reference(expected, code);
		std::size_t retired{};
		require(g::execute(built.program, actual, *tlb, retired) ==
			uml::cpu::EngineEvent::system_call);
		require(retired == 2U);
		same_state(actual, expected);
		++cases;
	}
}

void every_encoding()
{
	std::uint64_t seed = 0x2f1b'80a3'5de4'c971ULL;
	std::size_t cases{};
	for (unsigned sf = 0; sf < 2; ++sf)
	for (unsigned opc : {0U, 2U})
	for (unsigned immr = 0; immr < (sf != 0U ? 64U : 32U); ++immr)
	for (unsigned imms = 0; imms < (sf != 0U ? 64U : 32U); ++imms) {
		single(sf, opc, immr, imms, 5, 3, seed, cases);
		// The destination as its own source, and the zero register.
		single(sf, opc, immr, imms, 3, 3, seed, cases);
		single(sf, opc, immr, imms, 31, 4, seed, cases);
	}
	for (unsigned sf = 0; sf < 2; ++sf)
	for (unsigned opc : {0U, 2U})
		for (unsigned immr : {0U, 5U, 17U, 31U})
			for (unsigned imms : {0U, 7U, 16U, 31U})
				single(sf, opc, immr, imms, 5, 31, seed, cases);
	std::printf("bitfield shift: %zu encoding/state comparisons\n", cases);
}

/* ADD, then the bitfield instruction reading its result, then an ADD reading
 * the bitfield instruction's: with forwarding on, each edge selects the
 * source-forwarded entry, and the result is the reference's either way. */
void forwarded_edges()
{
	std::uint64_t seed = 0x7a3d'0c15'e9b2'4f68ULL;
	auto tlb = std::make_unique<uml::memory::Tlb>();
	std::size_t cases{};
	for (unsigned sf = 0; sf < 2; ++sf)
	for (unsigned opc : {0U, 2U})
	for (unsigned immr = 0; immr < (sf != 0U ? 64U : 32U); ++immr)
	for (unsigned imms = 0; imms < (sf != 0U ? 64U : 32U); ++imms) {
		const std::array code{add_x3_x1_x2, encode(sf, opc, immr, imms, 3, 4),
			add_x6_x4_x5, system_call};
		const auto enabled = g::Builder{}.build(base, code);
		const auto disabled = g::Builder{}.build(base, code, {},
			std::pmr::get_default_resource(), {}, {.scalar_forwarding = false});
		require(enabled.complete && disabled.complete);
		const auto on = enabled.program.words();
		const auto off = disabled.program.words();
		require(on.size() == off.size());
		const auto entry = expected_entry(sf, opc == 0U, immr, imms);
		const auto found = std::ranges::find(off, entry);
		require(found != off.end());
		const auto at = static_cast<std::size_t>(found - off.begin());
		const auto access = g::scalar_access(off.subspan(at, 2));
		require(access.contract != nullptr && access.contract->operand_words == 1U);
		require(on[at] == access.contract->entries[1]);
		require(on[at] != off[at]);
		// The consumer after it forwards from the bitfield result.
		const auto consumer = g::scalar_access(off.subspan(at + 2, 2));
		require(consumer.contract != nullptr);
		require(on[at + 2] == consumer.contract->entries[1]);
		for (unsigned trial = 0; trial < 2; ++trial) {
			auto expected = random_state(seed);
			auto with = expected, without = expected;
			run_reference(expected, code);
			std::size_t on_count{}, off_count{};
			require(g::execute(enabled.program, with, *tlb, on_count) ==
				uml::cpu::EngineEvent::system_call);
			require(g::execute(disabled.program, without, *tlb, off_count) ==
				uml::cpu::EngineEvent::system_call);
			require(on_count == 4U && off_count == 4U);
			same_state(with, expected);
			same_state(without, expected);
			++cases;
		}
	}
	std::printf("bitfield shift: %zu forwarded comparisons\n", cases);
}

/* A program translated for SVCR.SM = 1 may name the entries, as the table of
 * streaming entries lists them. */
void streaming_program()
{
	for (unsigned sf = 0; sf < 2; ++sf)
	for (unsigned opc : {0U, 2U})
	for (const auto shape : {std::array{0U, 0U}, std::array{3U, 11U},
		std::array{5U, 4U}, std::array{9U, 31U}, std::array{0U, 7U},
		std::array{12U, 12U}}) {
		const std::array code{encode(sf, opc, shape[0], shape[1], 5, 3),
			system_call};
		const auto built = g::Builder{}.build(base, code, {},
			std::pmr::get_default_resource(), {}, {.svcr = a::svcr_sm});
		require(built.complete);
		require(built.program.words()[0] ==
			expected_entry(sf, opc == 0U, shape[0], shape[1]));
	}
}

/* BFM keeps its mask words, so these four-word instructions still run. */
void bfm_unchanged()
{
	std::uint64_t seed = 0x51c9'6e2a'0b47'd3f8ULL;
	auto tlb = std::make_unique<uml::memory::Tlb>();
	for (unsigned sf = 0; sf < 2; ++sf)
	for (unsigned immr : {0U, 3U, 20U})
	for (unsigned imms : {2U, 9U, 30U}) {
		const std::array code{encode(sf, 1, immr, imms, 5, 3), system_call};
		const auto built = g::Builder{}.build(base, code);
		require(built.program.words().size() == 4U + system_call_words());
		auto expected = random_state(seed);
		auto actual = expected;
		run_reference(expected, code);
		require(g::execute(built.program, actual, *tlb) ==
			uml::cpu::EngineEvent::system_call);
		same_state(actual, expected);
	}
}

} // namespace

void run_gadget_bitfield_shift_tests()
{
	every_encoding();
	forwarded_edges();
	streaming_program();
	bfm_unchanged();
}
