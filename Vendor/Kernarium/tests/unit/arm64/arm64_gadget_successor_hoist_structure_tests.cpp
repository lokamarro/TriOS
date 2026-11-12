// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/gadget_symbols.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace selection = gadgets::selection;

namespace {

using uml::emulator::tests::Gadget;
using uml::testing::require;
using uml::emulator::tests::address;

[[nodiscard]] std::uint32_t instruction(Gadget gadget, std::size_t index)
{
	std::uint32_t result{};
	const auto *bytes = reinterpret_cast<const std::byte *>(address(gadget));
	std::memcpy(&result, bytes + index * sizeof(result), sizeof(result));
	return result;
}

struct Entry final {
	Gadget gadget;
	bool source_sp;
	bool flags;
	bool destination_special;
};

#define UML_ADD_SUB_IMMEDIATE_ENTRIES(ENTRY) \
	ENTRY(w_add_n_rr, false, false, false) \
	ENTRY(w_add_n_rs, false, false, true) \
	ENTRY(w_add_n_sr, true, false, false) \
	ENTRY(w_add_n_ss, true, false, true) \
	ENTRY(w_add_s_rr, false, true, false) \
	ENTRY(w_add_s_rs, false, true, true) \
	ENTRY(w_add_s_sr, true, true, false) \
	ENTRY(w_add_s_ss, true, true, true) \
	ENTRY(w_sub_n_rr, false, false, false) \
	ENTRY(w_sub_n_rs, false, false, true) \
	ENTRY(w_sub_n_sr, true, false, false) \
	ENTRY(w_sub_n_ss, true, false, true) \
	ENTRY(w_sub_s_rr, false, true, false) \
	ENTRY(w_sub_s_rs, false, true, true) \
	ENTRY(w_sub_s_sr, true, true, false) \
	ENTRY(w_sub_s_ss, true, true, true) \
	ENTRY(x_add_n_rr, false, false, false) \
	ENTRY(x_add_n_rs, false, false, true) \
	ENTRY(x_add_n_sr, true, false, false) \
	ENTRY(x_add_n_ss, true, false, true) \
	ENTRY(x_add_s_rr, false, true, false) \
	ENTRY(x_add_s_rs, false, true, true) \
	ENTRY(x_add_s_sr, true, true, false) \
	ENTRY(x_add_s_ss, true, true, true) \
	ENTRY(x_sub_n_rr, false, false, false) \
	ENTRY(x_sub_n_rs, false, false, true) \
	ENTRY(x_sub_n_sr, true, false, false) \
	ENTRY(x_sub_n_ss, true, false, true) \
	ENTRY(x_sub_s_rr, false, true, false) \
	ENTRY(x_sub_s_rs, false, true, true) \
	ENTRY(x_sub_s_sr, true, true, false) \
	ENTRY(x_sub_s_ss, true, true, true)

#define UML_ENTRY(suffix, source_sp, flags, destination_special) \
	Entry{selection::uml_arm64_gadget_add_sub_immediate_##suffix, \
		source_sp, flags, destination_special},
constexpr std::array<Entry, 32> entries{{
	UML_ADD_SUB_IMMEDIATE_ENTRIES(UML_ENTRY)
}};
#undef UML_ENTRY

[[nodiscard]] constexpr std::size_t branch_index(const Entry &entry) noexcept
{
	const std::size_t source_instructions = entry.source_sp ? 1U : 2U;
	const std::size_t flag_instructions = entry.flags ? 1U : 0U;
	const std::size_t destination_instructions = entry.destination_special ?
		(entry.flags ? 0U : 1U) : 2U;
	return 3U + source_instructions + flag_instructions +
		destination_instructions;
}

[[nodiscard]] constexpr std::size_t aligned_entry_bytes(
	std::size_t instruction_count) noexcept
{
	constexpr std::size_t alignment = 16U;
	const auto bytes = instruction_count * sizeof(std::uint32_t);
	return (bytes + alignment - 1U) & ~(alignment - 1U);
}

void all_32_entries_keep_shape_and_reserve_x14()
{
	constexpr std::uint32_t operand_successor_load_pair = 0xa8c1'3b88U;
	constexpr std::uint32_t source_register = 0x5308'310aU;
	constexpr std::uint32_t source_value = 0xf86a'782fU;
	constexpr std::uint32_t source_sp = 0xf940'7c2fU;
	constexpr std::uint32_t immediate_value = 0xd350'fd0bU;
	constexpr std::uint32_t nzcv = 0xd53b'421bU;
	constexpr std::uint32_t destination_register = 0x1200'1109U;
	constexpr std::uint32_t destination_value = 0xf829'7820U;
	constexpr std::uint32_t destination_sp = 0xf900'7c20U;
	constexpr std::uint32_t branch_x14 = 0xd61f'01c0U;
	constexpr std::uint32_t nop = 0xd503'201fU;

	for (std::size_t index = 0; index < entries.size(); ++index) {
		const auto &entry = entries[index];
		const auto begin = address(entry.gadget);
		const auto end = index + 1U < entries.size() ?
			address(entries[index + 1U].gadget) :
			address(selection::
				uml_arm64_gadget_add_sub_register_w_add_n_sh_n_rr_r);
		const auto branch = branch_index(entry);
		const auto expected_bytes = aligned_entry_bytes(branch + 1U);
		const auto expected_words = expected_bytes / sizeof(std::uint32_t);
		std::array<std::uint32_t, 12> expected{};
		expected.fill(nop);
		std::size_t word{};
		const auto append = [&](std::uint32_t encoded) {
			require(word < expected.size());
			expected[word++] = encoded;
		};

		append(operand_successor_load_pair);
		if (entry.source_sp) {
			append(source_sp);
		} else {
			append(source_register);
			append(source_value);
		}
		append(immediate_value);
		const bool bits64 = index >= 16U;
		const bool subtract = (index % 16U) >= 8U;
		auto arithmetic = (bits64 ? 0x8b0b'01e0U : 0x0b0b'01e0U);
		if (subtract)
			arithmetic |= 0x4000'0000U;
		if (entry.flags)
			arithmetic |= 0x2000'0000U;
		append(arithmetic);
		if (entry.flags)
			append(nzcv);
		if (entry.destination_special) {
			if (!entry.flags)
				append(destination_sp);
		} else {
			append(destination_register);
			append(destination_value);
		}
		require(word == branch);
		append(branch_x14);
		require(word <= expected_words);

		require((begin & 15U) == 0U);
		require(end > begin);
		require(end - begin == expected_bytes);
		/* Exact words pin every architected register field in the bounded
		 * family, including Rn/Rm/base fields outside bits 4:0. The sole x14
		 * uses are the intentional LDP Rt2 and final BR. */
		for (std::size_t current = 0; current < expected_words; ++current)
			require(instruction(entry.gadget, current) == expected[current]);
	}
}

void probe_shape_has_one_predecessor_and_eight_successors()
{
	constexpr std::uint64_t code_base = 0x86'0000;
	constexpr std::uint32_t add_x20_x20_1 = 0x9100'0694U;
	/* Keep this list in lockstep with the measured loop in
	 * guest_gadget_polymorphic_successor_loop.S. */
	constexpr std::array<std::uint32_t, 18> instructions{
		add_x20_x20_1, 0xd503'201fU, // nop
		add_x20_x20_1, 0xd282'4680U, // movz x0, #0x1234
		add_x20_x20_1, 0xf2aa'cf01U, // movk x1, #0x5678, lsl #16
		add_x20_x20_1, 0xca03'0442U, // eor x2, x2, x3, lsl #1
		add_x20_x20_1, 0xdac0'0c63U, // rev x3, x3
		add_x20_x20_1, 0xdac0'1084U, // clz x4, x4
		add_x20_x20_1, 0x93c5'1ca5U, // ror x5, x5, #7
		add_x20_x20_1, 0x9a1f'00c6U, // adc x6, x6, xzr
		0xeb12'0273U, // subs x19, x19, x18
		0x54ff'fde1U, // b.ne to the first ADD
	};
	constexpr std::array<std::size_t, 8> predecessor_words{
		0U, 3U, 7U, 11U, 15U, 19U, 23U, 27U,
	};
	constexpr std::array<std::size_t, 8> successor_words{
		2U, 5U, 9U, 13U, 17U, 21U, 25U, 29U,
	};

	const auto result = gadgets::Builder{}.build(
		code_base, instructions, {}, std::pmr::get_default_resource());
	const auto words = result.program.words();
	require(result.complete);
	require(result.program.instruction_count() == instructions.size());
	require(words.size() > successor_words.back());

	const auto predecessor = words[predecessor_words.front()];
	/* "add x20, x20, #1" is the in-place shape, so the baked-displacement
	 * entry for x20 is the one the translator selects. */
	require(predecessor == address(
		selection::uml_arm64_gadget_add_sub_immediate_inplace_table[
			((1U * 2U + 0U) * 2U + 0U) * 31U + 20U]));
	for (const auto word : predecessor_words)
		require(words[word] == predecessor);
	for (std::size_t index = 0; index < successor_words.size(); ++index) {
		const std::array successor{instructions[index * 2U + 1U]};
		const auto selected = gadgets::Builder{}.build(
			code_base, successor, {}, std::pmr::get_default_resource());
		require(selected.complete);
		require(words[successor_words[index]] ==
			selected.program.words().front());
	}
	for (std::size_t left = 0; left < successor_words.size(); ++left)
		for (std::size_t right = left + 1U;
		     right < successor_words.size(); ++right)
			require(words[successor_words[left]] !=
				words[successor_words[right]]);
}

#undef UML_ADD_SUB_IMMEDIATE_ENTRIES

} // namespace

int main()
{
	all_32_entries_keep_shape_and_reserve_x14();
	probe_shape_has_one_predecessor_and_eight_successors();
	return 0;
}
