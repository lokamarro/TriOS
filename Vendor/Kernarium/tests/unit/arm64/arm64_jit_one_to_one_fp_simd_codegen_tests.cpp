// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_one_to_one_codegen_test_support.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace contract = uml::emulator::tests::jit_one_to_one;

namespace {

constexpr std::uint64_t code_base = 0x5b00'0000ULL;
constexpr unsigned destination = 0U;
constexpr unsigned first = 1U;
constexpr unsigned second = 2U;
constexpr unsigned addend = 3U;
constexpr std::uint32_t unary_fields = first << 5U;
constexpr std::uint32_t binary_fields =
	(second << 16U) | unary_fields;
constexpr std::uint32_t fused_fields =
	binary_fields | (addend << 10U);

[[nodiscard]] constexpr std::uint32_t
modified_immediate(unsigned immediate) noexcept
{
	return 0x4f00'0400U | ((immediate & 0xe0U) << 11U) |
	       (14U << 12U) | ((immediate & 0x1fU) << 5U) |
	       destination;
}

[[nodiscard]] constexpr std::uint32_t
integer_indexed(bool q, bool unsigned_group, unsigned size,
		unsigned operation, unsigned destination_register,
		unsigned left, unsigned element, unsigned index) noexcept
{
	std::uint32_t instruction =
		0x0f00'0000U | (q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22U) | ((operation & 15U) << 12U) |
		((element & 15U) << 16U) | ((left & 31U) << 5U) |
		(destination_register & 31U);
	if (size == 1U) {
		instruction |= (index & 1U) << 20U;
		instruction |= (index & 2U) << 20U;
		instruction |= (index & 4U) << 9U;
	} else if (size == 2U) {
		instruction |= ((element >> 4U) & 1U) << 20U;
		instruction |= (index & 1U) << 21U;
		instruction |= (index & 2U) << 10U;
	}
	return instruction;
}

void scalar_floating_families_are_one_to_one()
{
	if (!::uml::platform::apple::probed_host_capabilities().floating_point)
		return;
	constexpr std::array cases{
		contract::Case{"basic floating arithmetic",
			0x1e20'2800U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"fused floating arithmetic",
			0x1f00'0000U | fused_fields,
			contract::ra_rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(10U, addend),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"integer-to-floating conversion",
			0x9e62'0000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::gpr(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"floating-to-integer conversion",
			0x9e78'0000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::gpr(0U, destination))},
		contract::Case{"floating precision conversion",
			0x1e22'c000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"floating round",
			0x1e24'4000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"floating unary",
			0x1e20'c000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"floating compare",
			0x1e20'2000U | binary_fields,
			(31U << 16U) | (31U << 5U), true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first))},
		contract::Case{"floating select",
			0x1e20'0c00U | binary_fields,
			contract::rm_rn_rd_mask, false,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"floating move immediate",
			0x1e20'1000U | (0x70U << 13U),
			contract::rd_mask, false,
			contract::vector_state_metadata,
			contract::operands(
				contract::vector(0U, destination))},
		contract::Case{"general-to-vector move",
			0x9e67'0000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::gpr(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector-to-general move",
			0x9e66'0000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::gpr(0U, destination))},
	};
	contract::require_all(cases, code_base);
}

void baseline_vector_families_are_one_to_one()
{
	if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
		return;
	constexpr std::array cases{
		contract::Case{"vector integer/floating conversion",
			0x5e79'd800U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector horizontal floating arithmetic",
			0x6e20'd400U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector reciprocal arithmetic",
			0x5e20'fc00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector floating compare",
			0x4e20'e400U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector bitwise register",
			0x4e20'1c00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector three-same integer",
			0x4ea0'8400U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector arithmetic narrow",
			0x0e20'4000U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector widening",
			0x0e20'0000U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector narrow move",
			0x0e21'2800U | unary_fields,
			contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector two-register integer",
			0x4e20'2800U | unary_fields,
			contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector unsigned estimate",
			0x4ea1'c800U | unary_fields,
			contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector indexed floating arithmetic",
			0x4f80'1000U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector extract",
			0x6e00'0800U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector table lookup",
			0x4e00'0000U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector permute",
			0x4e00'2800U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector extended multiply",
			0x4e20'dc00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector polynomial multiply",
			0x0e20'e000U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))},
	};
	contract::require_all(cases, code_base + 0x20000U);
}

void vector_lane_and_transform_families_are_one_to_one()
{
	if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
		return;
	constexpr std::array cases{
		contract::Case{"vector modified immediate",
			modified_immediate(0xa6U), contract::rd_mask, false,
			contract::vector_state_metadata,
			contract::operands(
				contract::vector(0U, destination))},
		contract::Case{"vector duplicate general",
			0x4e08'0c20U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::gpr(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector insert general",
			0x4e08'1c20U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::gpr(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector insert element",
			0x6e08'4420U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector duplicate element",
			0x4e04'0420U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector bit unary",
			0x4e20'5820U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector integer reduction",
			0x4e31'b820U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector scalar pairwise add",
			0x5ef1'b820U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector shift immediate",
			0x4f28'5420U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
		contract::Case{"vector shift-left-long",
			0x4f0c'a420U, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))},
	};
	contract::require_all(cases, code_base + 0x40000U);
}

void previously_uncovered_lowering_paths_are_one_to_one()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	std::uint64_t pc = code_base + 0x48000U;
	const auto check = [&](const contract::Case &test) {
		contract::require_one_to_one(test, pc);
		pc += 0x1000U;
	};

	if (host.floating_point) {
		check({"floating register move",
		       0x1e20'4000U | unary_fields,
		       contract::rn_rd_mask, true,
		       contract::vector_state_metadata,
		       contract::operands(contract::vector(5U, first),
					  contract::vector(0U, destination))});
		check({"floating compare with zero",
		       0x1e20'2008U | unary_fields,
		       31U << 5U, true, contract::native_fp_metadata,
		       contract::operands(contract::vector(5U, first))});
		check({"conditional floating compare",
		       0x1e22'0405U | unary_fields,
		       (31U << 16U) | (31U << 5U), true,
		       contract::native_fp_metadata,
		       contract::operands(contract::vector(16U, second),
					  contract::vector(5U, first))});
	}

	if (!host.advanced_simd)
		return;

	check({"integer vector indexed unrestricted",
	       integer_indexed(true, true, 2U, 0U, destination, first,
			       second, 1U),
	       contract::rm_rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(contract::vector(16U, second),
				  contract::vector(5U, first),
				  contract::vector(0U, destination))});
	check({"integer vector indexed constrained",
	       integer_indexed(true, false, 1U, 8U, destination, first,
			       second, 1U),
	       (15U << 16U) | contract::rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(
		       contract::constrained_vector(16U, 4U, second),
		       contract::vector(5U, first),
		       contract::vector(0U, destination))});
	check({"integer vector indexed constrained all-alias",
	       integer_indexed(true, false, 1U, 8U, first, first,
			       first, 1U),
	       (15U << 16U) | contract::rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(
		       contract::constrained_vector(16U, 4U, first),
		       contract::vector(5U, first),
		       contract::vector(0U, first))});
	check({"integer vector compare with zero",
	       0x4e20'8800U | unary_fields,
	       contract::rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(contract::vector(5U, first),
				  contract::vector(0U, destination))});
	check({"vector lane to reserved guest GPR",
	       0x4e08'3c00U | (first << 5U) | 18U,
	       contract::rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(contract::vector(5U, first),
				  contract::gpr(0U, 18U))});
	check({"reserved guest GPR to upper vector lane",
	       0x9eaf'0000U | (18U << 5U) | destination,
	       contract::rn_rd_mask, true,
	       contract::vector_state_metadata,
	       contract::operands(contract::gpr(5U, 18U),
				  contract::vector(0U, destination))});
}

void supported_arithmetic_extensions_are_one_to_one()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	std::uint64_t pc = code_base + 0x50000U;
	const auto check = [&](const contract::Case &test) {
		contract::require_one_to_one(test, pc);
		pc += 0x1000U;
	};

	if (host.has(::uml::cpu::ArmFeature::rdm))
		check({"rounding multiply/accumulate", 0x6e40'8400U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.has(::uml::cpu::ArmFeature::fcma))
		check({"complex floating arithmetic", 0x6e40'e400U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.has(::uml::cpu::ArmFeature::dotprod))
		check({"vector dot accumulator", 0x4e80'9400U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.has(::uml::cpu::ArmFeature::i8mm))
		check({"vector matrix accumulator", 0x4e80'a400U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});

	if (host.has(::uml::cpu::ArmFeature::bf16)) {
		check({"reduced-precision conversion",
			0x1e63'4000U | unary_fields,
			contract::rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))});
		check({"reduced-precision dot accumulator",
			0x6e40'fc00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
		check({"reduced-precision multiply-long accumulator",
			0x6ec0'fc00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
		check({"reduced-precision matrix accumulator",
			0x6e40'ec00U | binary_fields,
			contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	}
	if (host.has(::uml::cpu::ArmFeature::fhm))
		check({"half-width fused-long accumulator", 0x4e20'ec00U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::native_fp_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
}

void supported_vector_transform_extensions_are_one_to_one()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	std::uint64_t pc = code_base + 0x70000U;
	const auto check = [&](const contract::Case &test) {
		contract::require_one_to_one(test, pc);
		pc += 0x1000U;
	};

	if (host.aes)
		check({"optional vector transform 1", 0x4e28'4800U |
			unary_fields, contract::rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.sha1)
		check({"optional vector transform 2", 0x5e00'0000U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.sha2)
		check({"optional vector transform 3", 0x5e00'4000U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.sha512)
		check({"optional vector transform 4", 0xce60'8000U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
	if (host.sha3)
		check({"optional vector transform 5", 0xce60'8c00U |
			binary_fields, contract::rm_rn_rd_mask, true,
			contract::vector_state_metadata,
			contract::operands(contract::vector(16U, second),
					   contract::vector(5U, first),
					   contract::vector(0U, destination))});
}

void host_status_quirk_is_translation_selected()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	if (!host.has(::uml::cpu::ArmFeature::jscvt))
		return;

	constexpr contract::Case test{
		"status-corrected conversion", 0x1e7e'0000U | unary_fields,
		contract::rn_rd_mask, false, contract::native_fp_metadata,
		contract::operands(contract::vector(5U, first),
				   contract::gpr(0U, destination))};
	if (host.jscvt_subnormal_flags_correct) {
		contract::require_one_to_one(test, code_base + 0x7f000U);
		return;
	}

	/*
	 * This remains the sole ordinary compute exception on affected hosts.
	 * The host probe selects the shape before translation, so neither path
	 * contains a runtime feature decision.
	 */
	auto match = contract::compile_and_find(test, code_base + 0x7f000U);
	const auto hot = std::span<const std::uint32_t>{match.compiled.words}
				 .first(match.compiled.hot_words);
	contract::require(match.semantic_index + 7U <= match.successor_index,
		test.name, "status correction is truncated");
	const auto correction = hot.subspan(match.semantic_index + 1U, 6U);
	contract::require(
		(correction[0] & ~(31U << 5U)) == 0x9e66'000aU,
		test.name, "status correction lost its source transfer");
	contract::require(correction[1] == 0xd374'f94bU, test.name,
		"status correction exponent extraction changed");
	contract::require(correction[2] == 0xb500'008bU, test.name,
		"status correction first local branch changed");
	contract::require(correction[3] == 0xd374'cd4bU, test.name,
		"status correction magnitude test changed");
	contract::require(correction[4] == 0xb400'004bU, test.name,
		"status correction second local branch changed");
	contract::require(correction[5] == 0xd51b'421fU, test.name,
		"status correction flag update changed");

	std::size_t conditional_branches{};
	for (std::size_t index = match.semantic_index + 1U;
	     index < match.successor_index; ++index) {
		const auto instruction = hot[index];
		conditional_branches +=
			contract::is_conditional_runtime_branch(instruction);
		contract::require(
			!contract::is_runtime_control_transfer(instruction) ||
				index == match.semantic_index + 3U ||
				index == match.semantic_index + 5U,
			test.name,
			"undocumented control transfer entered status correction");
	}
	contract::require(conditional_branches == 2U, test.name,
		"the documented status correction shape changed");
}

} // namespace

int main()
{
	scalar_floating_families_are_one_to_one();
	baseline_vector_families_are_one_to_one();
	vector_lane_and_transform_families_are_one_to_one();
	previously_uncovered_lowering_paths_are_one_to_one();
	supported_arithmetic_extensions_are_one_to_one();
	supported_vector_transform_extensions_are_one_to_one();
	host_status_quirk_is_translation_selected();
	return 0;
}
