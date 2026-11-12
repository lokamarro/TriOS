// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <array>
#include <algorithm>
#include <span>

using namespace uml::emulator::tests;

namespace {

constexpr std::uint64_t feature_gate_pc = 0x4f90'0000;

[[nodiscard]] constexpr std::uint32_t pstate_immediate(
	unsigned immediate, unsigned op2) noexcept
{
	return 0xd503'401fU | ((immediate & 15U) << 8U) |
		((op2 & 7U) << 5U);
}

[[nodiscard]] bool is_host_key_operation(std::uint32_t instruction)
{
	if ((instruction & 0xffff'c000U) == 0xdac1'0000U ||
	    (instruction & 0xffff'f800U) == 0xdac1'4000U ||
	    (instruction & 0xffe0'fc00U) == 0x9ac0'3000U ||
	    (instruction & 0xfedf'f800U) == 0xd61f'0800U ||
	    (instruction & 0xffff'fbffU) == 0xd65f'0bffU ||
	    (instruction & 0xff20'0400U) == 0xf820'0400U)
		return true;
	if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
		return false;
	const unsigned immediate = (instruction >> 5) & 0x7fU;
	return immediate == 7U || immediate == 8U || immediate == 10U ||
		immediate == 12U || immediate == 14U ||
		(immediate >= 24U && immediate <= 31U);
}

/* The virtual CPU implements no FEAT_PAuth. Its non-hint encodings compile to
 * the same illegal terminal as an unknown word, and its hint-space forms to
 * the same code as NOP, so neither a host key nor added work reaches the
 * translation. */
void pointer_authentication_compiles_as_absent()
{
	constexpr std::uint64_t pc = 0x4fa0'0000;
	constexpr std::array undefined{
		0xdac1'0020U, // pacia x0, x1
		0xdac1'23e0U, // paciza x0
		0xdac1'43e0U, // xpaci x0
		0x9ac2'3020U, // pacga x0, x1, x2
		0xd61f'083fU, // braa x1, sp
		0xd63f'081fU, // blraaz x0
		0xd65f'0bffU, // retaa
		0xf820'0420U, // ldraa x0, [x1]
		0xf8a0'0c20U, // ldrab x0, [x1]!
	};
	const std::array unknown{0xffff'ffffU};
	const auto reference = uml::cpu::arm64::jit::testing::compile_block(
		pc, unknown);
	for (const auto instruction : undefined) {
		const std::array guest{instruction};
		const auto compiled =
			uml::cpu::arm64::jit::testing::compile_block(pc, guest);
		require(compiled.guest_instructions == 1);
		require(compiled.words == reference.words);
	}
	const std::array nop{0xd503'201fU, 0xd400'0001U};
	const auto nop_code =
		uml::cpu::arm64::jit::testing::compile_block(pc, nop);
	for (const auto immediate :
	     {7U, 8U, 10U, 12U, 14U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
	      39U}) {
		const std::array guest{0xd503'201fU | immediate << 5,
				       0xd400'0001U};
		const auto compiled =
			uml::cpu::arm64::jit::testing::compile_block(pc, guest);
		require(compiled.guest_instructions == 2);
		require(compiled.words == nop_code.words);
		for (const auto word : compiled.words)
			require(!is_host_key_operation(word));
	}
}

void require_translation_feature_gate(std::uint32_t instruction,
	uml::cpu::ArmFeature feature)
{
	const std::array guest{instruction};
	const auto unavailable =
		uml::cpu::arm64::jit::testing::compile_block_with_feature_override(
			feature_gate_pc, guest, feature, false);
	require(unavailable.guest_illegal);
	require(unavailable.guest_instructions == 1);
	require(!unavailable.native_fp);

	const auto available =
		uml::cpu::arm64::jit::testing::compile_block_with_feature_override(
			feature_gate_pc, guest, feature, true);
	require(!available.guest_illegal);
	require(available.guest_instructions == 1);
	require(!available.native_fp);
}

void stateful_system_instructions_are_translation_feature_gated()
{
	using uml::cpu::ArmFeature;

	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		require_translation_feature_gate(
			pstate_immediate(immediate, 2), ArmFeature::dit);
		/* MSR TCO stays UNDEFINED even on an MTE host: it compiles to
		 * the same illegal terminal as an unallocated field. */
		const std::array tco{pstate_immediate(immediate, 4)};
		const std::array unallocated{pstate_immediate(immediate, 0)};
		const auto compiled = uml::cpu::arm64::jit::testing::
			compile_block_with_feature_override(
				feature_gate_pc, tco, ArmFeature::mte, true);
		const auto reference = uml::cpu::arm64::jit::testing::
			compile_block_with_feature_override(
				feature_gate_pc, unallocated, ArmFeature::mte, true);
		require(compiled.guest_instructions == 1);
		require(compiled.words == reference.words);
	}

	/* The virtual CPU implements neither FEAT_SSBS nor FEAT_FP8: MSR SSBS
	 * and MRS and MSR of FPMR compile to the same illegal terminal as an
	 * unknown word, even on a host that has them. It implements FEAT_SME
	 * exactly when the host profile does (the guest-SME policy), whatever
	 * the translation's features say; without it, SMSTART and SMSTOP are
	 * that terminal too. */
	const auto require_absent = [](std::uint32_t instruction,
					  ArmFeature feature) {
		const std::array unknown{0xffff'ffffU};
		const std::array guest{instruction};
		const auto reference = uml::cpu::arm64::jit::testing::
			compile_block_with_feature_override(
				feature_gate_pc, unknown, feature, true);
		const auto compiled = uml::cpu::arm64::jit::testing::
			compile_block_with_feature_override(
				feature_gate_pc, guest, feature, true);
		require(compiled.guest_instructions == 1);
		require(compiled.words == reference.words);
	};
	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		require_absent(pstate_immediate(immediate, 1), ArmFeature::ssbs);
		if (!uml::cpu::arm64::implements_sme(
			    uml::cpu::host_capabilities()))
			require_absent(pstate_immediate(immediate, 3),
				ArmFeature::sme);
	}
	for (unsigned transfer = 0; transfer < 32; ++transfer)
		for (const std::uint32_t access : {0xd53b'4440U, 0xd51b'4440U})
			require_absent(access | transfer, ArmFeature::fp8);
}

void linked_blocks_use_persistent_internal_entry()
{
	constexpr std::uint64_t pc = 0x4fb0'0000;
	constexpr std::array guest{0x1400'0001U}; // b pc + 4
	const auto compiled =
		uml::cpu::arm64::jit::testing::compile_linked_block(pc, guest);
	constexpr std::array control_prologue{
		0xa9bc'7bfdU, // stp x29, x30, [sp, #-64]!
		0x9100'03fdU, // mov x29, sp
		0xa901'07e0U, // stp x0, x1, [sp, #16]
		0xa902'57e2U, // stp x2, x21, [sp, #32]
		0xa903'53f3U, // stp x19, x20, [sp, #48]
		0xaa00'03efU, // mov x15, x0
		0xaa01'03eeU, // mov x14, x1
		0xaa02'03f3U, // mov x19, x2
		0xd282'0014U, // movz x20, #4096
		0x3940'6275U, // ldrb w21, [x19, #24]
	};
	const auto prologue_words = control_prologue.size();
	require(compiled.guest_instructions == 1);
	require(compiled.internal_entry_word == prologue_words);
	require(compiled.words.size() >= prologue_words);
	require(std::equal(control_prologue.begin(), control_prologue.end(),
		compiled.words.begin()));

	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	const auto branch = std::find(hot.begin(), hot.end(), 0xd61f'01a0U);
	require(branch != hot.end()); // br x13 to the linked internal entry.
	const auto branch_word =
		static_cast<std::size_t>(branch - hot.begin());
	require(branch_word >= compiled.internal_entry_word);
	/* A successful transition must not reload the external arguments or
	 * unwind the owner frame before branching to its successor. */
	require(std::find(hot.begin(), branch, 0xf940'0be0U) == branch);
	require(std::find(hot.begin(), branch, 0xf940'0fe1U) == branch);
	require(std::find(hot.begin(), branch, 0xf940'13e2U) == branch);
	constexpr std::array control_epilogue{
		0xf900'0274U, // str x20, [x19]
		0xa943'53f3U, // ldp x19, x20, [sp, #48]
		0xf940'17f5U, // ldr x21, [sp, #40]
		0xa8c4'7bfdU, // ldp x29, x30, [sp], #64
		0xd65f'03c0U, // ret
	};
	const auto require_restoration = [&](const auto &epilogue) {
		for (const auto restoration : epilogue)
			require(std::find(hot.begin(), branch, restoration) == branch);
		require(std::search(hot.begin(), hot.end(), epilogue.begin(),
			epilogue.end()) != hot.end());
	};
	require_restoration(control_epilogue);
	require(uml::cpu::arm64::jit::testing::
			successful_link_host_word_savings() == 14);
}

void link_state_metadata_is_consistent()
{
	constexpr std::uint64_t pc = 0x4fc0'0000;
	constexpr std::array ordinary{0x8b02'0020U}; // add x0, x1, x2
	constexpr std::array vector_only{0x4ea2'8420U}; // add v0.4s,v1.4s,v2.4s
	constexpr std::array native_environment{
		0x1e62'2820U, // fadd d0, d1, d2
	};
	const auto plain =
		uml::cpu::arm64::jit::testing::compile_block(pc, ordinary);
	const auto vector =
		uml::cpu::arm64::jit::testing::compile_block(
			pc + 0x1000, vector_only);
	const auto native =
		uml::cpu::arm64::jit::testing::compile_block(
			pc + 0x2000, native_environment);

	require(!plain.accesses_fp_state && !plain.native_fp);
	require(vector.accesses_fp_state && !vector.native_fp);
	require(native.accesses_fp_state && native.native_fp);
	for (const auto *compiled : std::array{&plain, &vector, &native})
		require(compiled->accesses_fp_state || !compiled->native_fp);
}

void link_selector_layout_is_dense_and_compact()
{
	constexpr std::array<std::uint8_t, 6> capability_masks{
		0, 1, 2, 3, 6, 7,
	};
	require(uml::cpu::arm64::jit::testing::
			link_capability_selector_count() ==
		capability_masks.size());
	for (std::size_t selector = 0;
	     selector < capability_masks.size(); ++selector) {
		require(uml::cpu::arm64::jit::testing::
				link_selector_for_capability_mask(
					capability_masks[selector]) ==
			selector);
		require(uml::cpu::arm64::jit::testing::
				link_capability_mask_for_selector(selector) ==
			capability_masks[selector]);
	}
	for (std::uint8_t capabilities = 0; capabilities < 16;
	     ++capabilities) {
		const bool valid = std::find(capability_masks.begin(),
			capability_masks.end(), capabilities) !=
			capability_masks.end();
		if (valid) {
			require(uml::cpu::arm64::jit::testing::
				checked_link_selector_for_capability_mask(
					capabilities) ==
				uml::cpu::arm64::jit::testing::
					link_selector_for_capability_mask(
						capabilities));
			continue;
		}
		require(uml::cpu::arm64::jit::testing::
				link_selector_for_capability_mask(
					capabilities) == 0xffU);
		require(uml::cpu::arm64::jit::testing::
				checked_link_selector_for_capability_mask(
					capabilities) == 0U);
	}
	require(uml::cpu::arm64::jit::testing::
			link_capability_mask_for_selector(
				capability_masks.size()) == 0xffU);
	require(uml::cpu::arm64::jit::testing::block_link_record_size() == 64);
	require(uml::cpu::arm64::jit::testing::
			block_link_record_alignment() == 64);
	require(uml::cpu::arm64::jit::testing::
			block_link_internal_entry_offset() == 16);
	require(uml::cpu::arm64::jit::testing::
			indirect_link_entry_size() == 16);
}

void tracked_translation_has_no_successor_branch()
{
	constexpr std::uint64_t pc = 0x4fc4'0000;
	constexpr std::array guest{0x1400'0001U}; // b pc + 4
	const auto compiled =
		uml::cpu::arm64::jit::testing::compile_tracked_block(pc, guest);
	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	require(std::find(hot.begin(), hot.end(), 0xd61f'01a0U) == hot.end());
}

void conditional_direct_link_capacity_refusal_is_observable()
{
	constexpr std::uint64_t pc = 0x4fc5'0000;
	constexpr std::array guest{0x5400'0040U}; // b.eq pc + 8
	const auto unlinked =
		uml::cpu::arm64::jit::testing::compile_block(pc, guest);
	require(unlinked.direct_link_refusals == 0);

	const auto complete = uml::cpu::arm64::jit::testing::
		compile_linked_block_with_capacity(pc, guest, 2);
	require(!complete.words.empty());
	require(complete.guest_instructions == guest.size());
	require(complete.direct_link_refusals == 0);

	const auto one_missing = uml::cpu::arm64::jit::testing::
		compile_linked_block_with_capacity(pc, guest, 1);
	require(!one_missing.words.empty());
	require(one_missing.guest_instructions == guest.size());
	require(one_missing.direct_link_refusals == 1);

	const auto both_missing = uml::cpu::arm64::jit::testing::
		compile_linked_block_with_capacity(pc, guest, 0);
	require(!both_missing.words.empty());
	require(both_missing.guest_instructions == guest.size());
	require(both_missing.direct_link_refusals == 2);
}

void indirect_link_capacity_refusal_is_observable()
{
	constexpr std::uint64_t pc = 0x4fc6'0000;
	constexpr std::array guest{0xd61f'00a0U}; // br x5
	const auto unlinked =
		uml::cpu::arm64::jit::testing::compile_block(pc, guest);
	require(unlinked.indirect_link_refusals == 0);

	const auto complete = uml::cpu::arm64::jit::testing::
		compile_indirect_linked_block_with_capacity(pc, guest, 1);
	require(!complete.words.empty());
	require(complete.guest_instructions == guest.size());
	require(complete.indirect_link_refusals == 0);

	const auto missing = uml::cpu::arm64::jit::testing::
		compile_indirect_linked_block_with_capacity(pc, guest, 0);
	require(!missing.words.empty());
	require(missing.guest_instructions == guest.size());
	require(missing.indirect_link_refusals == 1);
}

} // namespace

int main()
{
	require(jit_available() == 1);
	link_selector_layout_is_dense_and_compact();
	conditional_direct_link_capacity_refusal_is_observable();
	indirect_link_capacity_refusal_is_observable();
	require(uml::cpu::arm64::jit::testing::block_link_table_is_bounded(1));
	require(uml::cpu::arm64::jit::testing::block_link_table_is_bounded(7));
	linked_blocks_use_persistent_internal_entry();
	link_state_metadata_is_consistent();
	tracked_translation_has_no_successor_branch();
	pointer_authentication_compiles_as_absent();
	stateful_system_instructions_are_translation_feature_gated();
	require(uml::cpu::arm64::jit::testing::
		invalid_emitter_metadata_is_rejected_for_testing());
	return 0;
}
