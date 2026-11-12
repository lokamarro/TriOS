// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_one_to_one_codegen_test_support.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstdint>

namespace contract = uml::emulator::tests::jit_one_to_one;

namespace {

constexpr std::uint64_t code_base = 0x5a00'0000ULL;

[[nodiscard]] constexpr std::uint32_t
rotate_mask_flags(unsigned source, unsigned rotation,
		  unsigned mask) noexcept
{
	return 0xba00'0400U | ((rotation & 63U) << 15U) |
	       ((source & 31U) << 5U) | (mask & 15U);
}

[[nodiscard]] constexpr std::uint32_t
set_flags_from_register(bool sixteen, unsigned source) noexcept
{
	return 0x3a00'080dU | (sixteen ? 0x4000U : 0U) |
	       ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
unary(std::uint32_t operation, unsigned destination,
      unsigned source) noexcept
{
	return operation | ((source & 31U) << 5U) |
	       (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
ternary(std::uint32_t operation, unsigned destination, unsigned left,
	unsigned right) noexcept
{
	return operation | ((right & 31U) << 16U) |
	       ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
four_register(std::uint32_t operation, unsigned destination,
		      unsigned left, unsigned right, unsigned addend) noexcept
{
	return operation | ((right & 31U) << 16U) |
	       ((addend & 31U) << 10U) | ((left & 31U) << 5U) |
		       (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
conditional_compare_immediate(bool wide, bool subtract, unsigned condition,
			      unsigned alternative, unsigned immediate) noexcept
{
	return 0x3a40'0800U | (wide ? 0x8000'0000U : 0U) |
	       (subtract ? 0x4000'0000U : 0U) |
	       ((immediate & 31U) << 16U) |
	       ((condition & 15U) << 12U) | (31U << 5U) |
	       (alternative & 15U);
}

[[nodiscard]] constexpr std::uint32_t
pc_relative(bool page, std::int64_t immediate, unsigned destination) noexcept
{
	const auto encoded =
		static_cast<std::uint64_t>(immediate) & 0x1f'ffffU;
	return (page ? 0x9000'0000U : 0x1000'0000U) |
	       ((encoded & 3U) << 29U) | ((encoded >> 2U) << 5U) |
	       (destination & 31U);
}

void baseline_compute_families_are_one_to_one()
{
	/*
	 * These cases select distinct, nonzero guest registers so matching the
	 * opcode bits also proves that the native word names allocator-selected
	 * host registers. Flag-consuming forms keep their architectural flag
	 * import, but still may not branch or call a helper before the operation.
	 */
	constexpr std::array cases{
		contract::Case{"move-wide", 0xd282'4685U,
			contract::rd_mask, false, contract::no_fp_metadata,
			contract::operands(contract::gpr(0U, 5U))},
		contract::Case{"add/sub immediate", 0x9104'8c25U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"add/sub shifted", 0x8b02'0025U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"add/sub extended", 0x8b22'0025U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"add/sub with carry", 0x9a02'0025U,
			contract::rm_rn_rd_mask, false,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"logical immediate", 0x9240'1c25U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"bitfield", 0xd348'3c25U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"extract", 0x93c2'2025U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"logical shifted", 0xaa02'0025U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"conditional select", 0x9a82'0025U,
			contract::rm_rn_rd_mask, false,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"conditional compare", 0xfa42'002aU,
			(31U << 16U) | (31U << 5U), false,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U))},
		contract::Case{"multiply/add", 0x9b02'1825U,
			contract::ra_rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(10U, 6U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"two-source data processing", 0x9ac2'0825U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
		contract::Case{"one-source data processing", 0xdac0'0c25U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))},
	};
	contract::require_all(cases, code_base);
}

void stack_zero_and_reserved_register_boundaries_are_remapped()
{
	constexpr auto add_immediate = 0x9104'8c00U;
	constexpr auto logical_immediate = 0x9240'1c00U;
	constexpr std::array cases{
		contract::Case{
			"add immediate reads guest SP",
			add_immediate | (31U << 5U) | 18U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::sp(5U),
					   contract::gpr(0U, 18U))},
		contract::Case{
			"add immediate writes guest SP",
			add_immediate | (18U << 5U) | 31U,
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 18U),
					   contract::sp(0U))},
		contract::Case{
			"extended add aliases guest SP",
			ternary(0x8b20'0000U, 31U, 31U, 18U),
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 18U),
					   contract::sp(5U), contract::sp(0U))},
		contract::Case{
			"logical immediate preserves zero and remaps reserved destination",
			logical_immediate | (31U << 5U) | 18U,
			contract::rn_rd_mask, false,
			contract::no_fp_metadata,
			contract::operands(contract::zr(5U),
					   contract::gpr(0U, 18U))},
		contract::Case{
			"unary reserved-register boundary",
			unary(0xdac0'0c00U, 14U, 18U),
			contract::rn_rd_mask, true, contract::no_fp_metadata,
			contract::operands(contract::gpr(5U, 18U),
					   contract::gpr(0U, 14U))},
		contract::Case{
			"ternary reserved-register boundary",
			ternary(0x8b00'0000U, 18U, 14U, 15U),
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 15U),
					   contract::gpr(5U, 14U),
					   contract::gpr(0U, 18U))},
		contract::Case{
			"four-register reserved-register boundary",
			four_register(0x9b00'0000U, 18U, 14U, 15U, 16U),
			contract::ra_rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 15U),
					   contract::gpr(10U, 16U),
					   contract::gpr(5U, 14U),
					   contract::gpr(0U, 18U))},
	};
	contract::require_all(cases, code_base + 0x8000U);
}

void unconditional_zero_source_immediate_compares_stay_native()
{
	/*
	 * ADD/SUB immediate interpret register 31 as SP, so this alias used to
	 * materialize zero before an unconditional arithmetic instruction. The
	 * original CCMN/CCMP word already names ZR and both AL/NV are always true;
	 * require that every width, operation, condition, and imm5 value remains
	 * exactly one native semantic word.
	 */
	std::uint64_t pc = code_base + 0x20'000U;
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const unsigned condition : {14U, 15U})
				for (unsigned immediate = 0; immediate < 32U;
				     ++immediate) {
					const auto instruction =
						conditional_compare_immediate(
							wide, subtract,
							condition, 0xbU,
							immediate);
					const contract::Case test{
						"unconditional immediate compare with ZR",
						instruction, 0U, false,
						contract::no_fp_metadata};
					contract::require_one_to_one(test, pc);
					pc += 0x1000U;
				}
}

void pc_relative_constants_are_one_literal_load()
{
	/*
	 * The native code address is unrelated to the guest PC, so ADR/ADRP
	 * cannot retain their original PC-relative encoding. Resolve every value
	 * while translating and require one native literal load per instruction;
	 * the guest-derived values remain integers and never become native branch
	 * targets or unchecked host addresses.
	 *
	 * Reusing one destination keeps it resident and also exercises a pool
	 * larger than the two link pointers needed by an ordinary block.
	 */
	constexpr std::size_t instruction_count = 32U;
	constexpr unsigned destination = 5U;
	constexpr auto pc = code_base + 0x40'000U;
	std::array<std::uint32_t, instruction_count> guest{};
	std::array<std::uint64_t, instruction_count> expected{};
	for (std::size_t index = 0; index < guest.size(); ++index) {
		const bool page = (index & 1U) != 0U;
		const auto immediate =
			static_cast<std::int64_t>(index) * 257 - 4000;
		guest[index] = pc_relative(page, immediate, destination);
		const auto instruction_pc = pc + index * sizeof(std::uint32_t);
		const auto base = page ? instruction_pc & ~std::uint64_t{0xfff}
				       : instruction_pc;
		expected[index] = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(base) +
			immediate * (page ? 4096 : 1));
	}

	const auto compiled = contract::jit_test::compile_block(pc, guest);
	contract::require(compiled.guest_instructions == guest.size(),
		"PC-relative literal lowering",
		"JIT did not compile the complete PC-relative sequence");
	contract::require(!compiled.guest_illegal,
		"PC-relative literal lowering",
		"supported PC-relative instruction was classified as illegal");
	contract::require(
		compiled.internal_entry_word + guest.size() <=
			compiled.hot_words,
		"PC-relative literal lowering",
		"generated hot region is shorter than the guest sequence");

	unsigned native_destination = 32U;
	for (std::size_t index = 0; index < guest.size(); ++index) {
		const auto word =
			compiled.words[compiled.internal_entry_word + index];
		contract::require((word & 0xff00'0000U) == 0x5800'0000U,
			"PC-relative literal lowering",
			"ADR/ADRP expanded beyond one native literal load");
		const auto current_destination = word & 31U;
		if (index == 0)
			native_destination = current_destination;
		contract::require(
			current_destination == native_destination &&
				current_destination != destination &&
				current_destination < 14U,
			"PC-relative literal lowering",
			"literal result did not retain one safe remapped register");

		auto displacement =
			static_cast<std::int32_t>((word >> 5U) & 0x7ffffU);
		if ((displacement & (1 << 18U)) != 0)
			displacement |= ~0x7ffff;
		const auto source =
			static_cast<std::int64_t>(
				compiled.internal_entry_word + index);
		const auto target = source + displacement;
		contract::require(
			target >=
					static_cast<std::int64_t>(
						compiled.hot_words) &&
				target >= 0 &&
				static_cast<std::size_t>(target) + 1U <
					compiled.words.size() &&
				(static_cast<std::size_t>(target) & 1U) == 0U,
			"PC-relative literal lowering",
			"literal target is executable, unaligned, or out of range");
		const auto literal =
			static_cast<std::uint64_t>(
				compiled.words[static_cast<std::size_t>(target)]) |
			(static_cast<std::uint64_t>(
				 compiled.words[static_cast<std::size_t>(target) +
						1U])
			 << 32U);
		contract::require(literal == expected[index],
			"PC-relative literal lowering",
			"translation-time PC-relative value is incorrect");
	}
}

void supported_extension_families_are_one_to_one()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	std::uint64_t pc = code_base + 0x10000U;

	if (host.cssc) {
		constexpr std::array cases{
			contract::Case{"register min/max", 0x9ac2'6025U,
				contract::rm_rn_rd_mask, true,
				contract::no_fp_metadata,
				contract::operands(contract::gpr(16U, 2U),
						   contract::gpr(5U, 1U),
						   contract::gpr(0U, 5U))},
			contract::Case{"immediate min/max", 0x91c0'1c25U,
				contract::rn_rd_mask, true,
				contract::no_fp_metadata,
				contract::operands(contract::gpr(5U, 1U),
						   contract::gpr(0U, 5U))},
		};
		contract::require_all(cases, pc);
		pc += 0x2000U;
	}
	if (host.crc32) {
		constexpr contract::Case test{
			"checksum", 0x9ac2'4c25U,
			contract::rm_rn_rd_mask, true,
			contract::no_fp_metadata,
			contract::operands(contract::gpr(16U, 2U),
					   contract::gpr(5U, 1U),
					   contract::gpr(0U, 5U))};
		contract::require_one_to_one(test, pc);
		pc += 0x1000U;
	}
	if (host.has(::uml::cpu::ArmFeature::flag_m)) {
		constexpr std::array cases{
			contract::Case{
				"rotate/mask flag manipulation",
				rotate_mask_flags(18U, 37U, 0xbU),
				31U << 5U, false,
				contract::no_fp_metadata,
				contract::operands(
					contract::gpr(5U, 18U))},
			contract::Case{
				"register flag manipulation",
				set_flags_from_register(true, 18U),
				31U << 5U, false,
				contract::no_fp_metadata,
				contract::operands(
					contract::gpr(5U, 18U))},
		};
		contract::require_all(cases, pc);
	}
}

} // namespace

int main()
{
	baseline_compute_families_are_one_to_one();
	stack_zero_and_reserved_register_boundaries_are_remapped();
	unconditional_zero_source_immediate_compares_stay_native();
	pc_relative_constants_are_one_literal_load();
	supported_extension_families_are_one_to_one();
	return 0;
}
