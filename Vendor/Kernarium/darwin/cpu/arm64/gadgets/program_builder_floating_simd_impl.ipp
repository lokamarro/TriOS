// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

#include "../detail/simd_floating_classes.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

namespace {

struct ResidentMla final {
	Gadget gadget;
	unsigned destination;
	unsigned left;
	unsigned right;
};

[[nodiscard]] bool decode_resident_mla(
	std::uint32_t instruction, ResidentMla &decoded) noexcept
{
	if ((instruction & 0x9f20'0400U) != 0x0e20'0400U ||
	    (instruction & 0x1000'0000U) != 0U ||
	    ((instruction >> 11) & 0x1fU) != 18U ||
	    (instruction & 0x2000'0000U) != 0U)
		return false;

	const auto destination = instruction & 31U;
	const auto left = (instruction >> 5) & 31U;
	const auto right = (instruction >> 16) & 31U;
	if (destination >= 8U || left == destination || right == destination)
		return false;

	const auto gadget = simd_destination_resident_mla_gadget(
		(instruction & 0x4000'0000U) != 0U,
		(instruction >> 22) & 3U);
	if (gadget == nullptr)
		return false;

	decoded = {gadget, destination, left, right};
	return true;
}

[[nodiscard]] bool emit_resident_mla_run(DecodeContext &context)
{
	ResidentMla first{};
	if (!decode_resident_mla(context.instruction, first))
		return false;

	std::size_t run_length = 1;
	ResidentMla decoded{};
	while (run_length <= context.following.size() &&
	       decode_resident_mla(context.following[run_length - 1], decoded) &&
	       decoded.destination == first.destination)
		++run_length;
	if (run_length < 4U)
		return false;

	const auto destination = vector_operand_offset(first.destination);
	emit(context.words, uml_arm64_gadget_simd_destination_resident_load);
	emit(context.words, destination);
	for (std::size_t index = 0; index < run_length; ++index) {
		const auto instruction = index == 0 ? context.instruction :
			context.following[index - 1];
		(void)decode_resident_mla(instruction, decoded);
		emit(context.words, decoded.gadget);
		emit(context.words,
			vector_operand_offset(decoded.left) |
			(static_cast<std::uintptr_t>(
				vector_operand_offset(decoded.right)) << 16U));
	}
	emit(context.words, uml_arm64_gadget_simd_destination_resident_flush);
	emit(context.words, destination);
	context.compiled += run_length - 1;
	return true;
}

} // namespace

DecodeResult decode_floating_simd(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &words = context.words;
	const auto &options = context.options;

	/* The class decoders below match only the bits that separate their own
	 * allocations, so a word in an unallocated region of the class table
	 * must stop here rather than reach the nearest of them. */
	if (detail::unallocated_simd_floating_class(instruction))
		return illegal_instruction(context);
	if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U) {
		if (!host_capabilities().has(ArmFeature::jscvt)) {
			emit(words,
				uml_arm64_gadget_floating_javascript_convert_software_fallback);
		} else {
			emit(words, (instruction & 31U) == 31U ?
				uml_arm64_gadget_floating_javascript_convert_discard :
				uml_arm64_gadget_floating_javascript_convert);
		}
		emit(words, (instruction & 31U) |
			(static_cast<std::uintptr_t>(
				(instruction >> 5) & 31U) << 8));
	} else if (const auto integer_to_floating =
			detail::decode_integer_to_floating(instruction);
		   integer_to_floating.recognized) {
		if (!integer_to_floating.valid)
			return illegal_instruction(context);
		emit(words, integer_to_floating_gadget(integer_to_floating,
			integer_to_floating.source_gpr &&
			((instruction >> 5) & 31U) == 31U));
		emit(words, detail::integer_to_floating_operand(
			instruction, integer_to_floating));
	} else if (const auto floating_to_integer =
			detail::decode_floating_to_integer(instruction);
		   floating_to_integer.recognized) {
		if (!floating_to_integer.valid)
			return illegal_instruction(context);
		emit(words, floating_to_integer_gadget(floating_to_integer,
			floating_to_integer.destination_gpr &&
			(instruction & 31U) == 31U));
		emit(words, detail::floating_to_integer_operand(
			instruction, floating_to_integer));
	} else if ((instruction & 0xff20'fc00U) == 0x1e20'2000U) {
		const auto type = (instruction >> 22) & 3U;
		const bool zero = (instruction & 8U) != 0U;
		if (type == 2U || (instruction & 7U) != 0U ||
		    (zero && ((instruction >> 16) & 31U) != 0U))
			return illegal_instruction(context);
		emit(words, floating_compare_gadget(
			type, (instruction & 16U) != 0U, zero));
		emit(words,
			(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
			(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
	} else if ((instruction & 0xff20'0c00U) == 0x1e20'0400U) {
		const auto type = (instruction >> 22) & 3U;
		if (type == 2U)
			return illegal_instruction(context);
		emit(words, floating_conditional_compare_gadget(
			(instruction >> 12) & 15U, type, (instruction & 16U) != 0U));
		emit(words, (instruction & 15U) |
			(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
			(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
	} else if ((instruction & 0xbf3f'fc00U) == 0x2e21'7800U ||
		   (instruction & 0xbfa0'fc00U) == 0x0e00'f400U ||
		   (instruction & 0xbf80'f400U) == 0x0f00'0000U ||
		   (instruction & 0xbfa0'fc00U) == 0x0e00'fc00U ||
		   (instruction & 0xbfc0'f400U) == 0x0fc0'0000U ||
		   (instruction & 0xbfe0'fc00U) == 0x0ec0'fc00U ||
		   (instruction & 0xbf80'f400U) == 0x2f00'8000U ||
		   (instruction & 0xbfa0'fc00U) == 0x0e00'c400U ||
		   (instruction & 0xff60'fc00U) == 0x6e00'ec00U) {
		/* The FEAT_FP8 conversions, dot products, multiply-adds, and
		 * matrix multiplies: the virtual CPU does not implement FEAT_FP8. */
		return illegal_instruction(context);
	} else if (const auto bfloat =
			detail::decode_bfloat_conversion(instruction);
		   bfloat.recognized) {
		emit(words, bfloat_conversion_gadget(bfloat));
		const auto destination = vector_operand_offset(instruction & 31U);
		const auto source =
			vector_operand_offset((instruction >> 5) & 31U);
		emit(words, destination |
			(static_cast<std::uintptr_t>(source) << 16U) |
			(static_cast<std::uintptr_t>(bfloat.scalar) << 32U) |
			(static_cast<std::uintptr_t>(bfloat.upper) << 33U));
	} else if (const auto bfloat_dot = detail::decode_bfloat_dot(instruction);
		   bfloat_dot.recognized) {
		emit(words, bfloat_dot_gadget(bfloat_dot));
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) & 31U,
			static_cast<unsigned>(bfloat_dot.by_element) |
				(static_cast<unsigned>(bfloat_dot.wide) << 1U) |
				(bfloat_dot.index << 2U)));
	} else if (detail::decode_bfloat_matrix_multiply(instruction)) {
		emit(words, bfloat_matrix_gadget());
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) & 31U));
	} else if (const auto bfloat_multiply =
			detail::decode_bfloat_multiply_long(instruction);
		   bfloat_multiply.recognized) {
		emit(words, bfloat_multiply_long_gadget(bfloat_multiply));
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) &
				(bfloat_multiply.by_element ? 15U : 31U),
			static_cast<unsigned>(bfloat_multiply.top) |
				(static_cast<unsigned>(
					bfloat_multiply.by_element) << 1U) |
				(bfloat_multiply.index << 2U)));
	} else if (((instruction & 0xbf20'fc00U) ==
			    0x2e00'fc00U &&
		    ((((instruction >> 22) & 3U) == 0U) ||
		     (((instruction >> 22) & 3U) == 2U))) ||
		   ((instruction & 0xbf20'fc00U) ==
			    0x2e00'ec00U &&
		    ((((instruction & 0x4000'0000U) == 0U) &&
		      (((instruction >> 22) & 3U) == 1U)) ||
		     (((instruction & 0x4000'0000U) != 0U) &&
		      (((instruction >> 22) & 3U) == 3U))))) {
		return illegal_instruction(context);
	} else if (const auto half_fused =
			detail::decode_half_fused_long(instruction);
		   half_fused.recognized) {
		emit(words, half_fused_long_gadget(half_fused));
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) &
				(half_fused.by_element ? 15U : 31U),
			static_cast<unsigned>(half_fused.by_element) |
				(static_cast<unsigned>(half_fused.subtract)
					<< 1U) |
				(static_cast<unsigned>(half_fused.upper)
					<< 2U) |
				(static_cast<unsigned>(half_fused.wide)
					<< 3U) |
				(half_fused.index << 4U)));
	} else if (detail::reserved_half_fused_long(instruction)) {
		return illegal_instruction(context);
	} else if ((instruction & 0x9fe0'fc00U) == 0x0ec0'ec00U ||
		   (instruction & 0xbfe0'fc00U) == 0x0e40'ec00U ||
		   (((instruction & 0xafe0'fc00U) == 0x0e80'fc00U ||
		     (instruction & 0xafc0'f400U) == 0x0f40'9000U) &&
		    ((instruction & 0x1000'0000U) == 0U ||
		     !detail::decode_fused_floating_arithmetic(
			     instruction).recognized))) {
		/* The FEAT_F16MM and FEAT_F16F32MM FMMLA and the FEAT_F16F32DOT
		 * FDOT: the virtual CPU implements none of them. Bit 28 set moves
		 * the FDOT fields into the scalar classes; the fused decoder below
		 * owns the words it recognizes there, and the rest are reserved. */
		return illegal_instruction(context);
	} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U &&
		   ((instruction >> 15) & 0x3fU) >= 4U &&
		   ((instruction >> 15) & 0x3fU) <= 7U) {
		const auto source_type = (instruction >> 22) & 3U;
		const auto destination_type = (instruction >> 15) & 3U;
		if (source_type == 2U || destination_type == 2U ||
		    source_type == destination_type)
			return illegal_instruction(context);
		emit(words, floating_convert_precision_gadget(
			source_type, destination_type));
		emit(words, simd_unary_operand(instruction & 31U,
			(instruction >> 5) & 31U));
	} else if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
		const auto type = (instruction >> 22) & 3U;
		if (type == 2U)
			return illegal_instruction(context);
		const auto immediate = static_cast<std::uint8_t>(
			(instruction >> 13) & 0xffU);
		const unsigned bitsize = detail::floating_width_from_type(type);
		const auto bits = expand_vfp_immediate(immediate, bitsize);
		emit(words, uml_arm64_gadget_floating_move_immediate);
		emit(words, instruction & 31U);
		emit(words, bits);
	} else if (const auto floating_complex =
			detail::decode_floating_complex(instruction);
		   floating_complex.recognized) {
		const auto gadget = floating_complex.valid ?
			floating_complex_gadget(floating_complex) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		emit(words, floating_complex_operand(
			instruction, floating_complex));
	} else if ((instruction & 0xbfe0'fc00U) == 0x2ec0'3c00U ||
		   (instruction & 0xbfa0'fc00U) == 0x2ea0'fc00U) {
		/* FSCALE belongs to FEAT_FP8, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if (const auto floating_reciprocal =
			detail::decode_floating_reciprocal(instruction);
		   floating_reciprocal.recognized) {
		const auto gadget = floating_reciprocal.valid ?
			floating_reciprocal_gadget(floating_reciprocal) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		emit(words, detail::floating_reciprocal_operand(
			instruction, floating_reciprocal));
	} else if (const auto floating_horizontal =
			detail::decode_floating_horizontal(instruction);
		   floating_horizontal.recognized) {
		const auto gadget = floating_horizontal.valid ?
			floating_horizontal_gadget(floating_horizontal) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		const bool uses_second =
			floating_horizontal.form ==
				detail::FloatingHorizontalForm::scalar_binary ||
		    floating_horizontal.form ==
				detail::FloatingHorizontalForm::vector_binary ||
		    floating_horizontal.form ==
				detail::FloatingHorizontalForm::vector_pairwise;
		if (uses_second)
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U));
		else
			emit(words, simd_unary_operand(
				instruction & 31U, (instruction >> 5) & 31U));
	} else if (const auto floating_round =
			detail::decode_floating_round(instruction);
		   floating_round.recognized) {
		if (!floating_round.valid)
			return illegal_instruction(context);
		emit(words, floating_round_gadget(floating_round));
		emit(words, detail::is_sized_round(floating_round.operation) ?
			detail::sized_floating_round_operand(
				instruction, floating_round) :
			detail::floating_round_operand(instruction));
	} else if (const auto fused_floating =
			detail::decode_fused_floating_arithmetic(instruction);
		   fused_floating.recognized) {
		const auto gadget = fused_floating.valid ?
			fused_floating_arithmetic_gadget(fused_floating,
				instruction & 31U) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		if (fused_floating.scalar)
			emit(words, simd_quaternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U,
				(instruction >> 10) & 31U));
		else
			emit(words, simd_ternary_operand(
				instruction & 31U, (instruction >> 5) & 31U,
				(instruction >> 16) & 31U));
	} else if (const auto basic_floating =
			detail::decode_basic_floating_arithmetic(instruction);
		   basic_floating.recognized) {
		const auto gadget = basic_floating.valid ?
			basic_floating_arithmetic_gadget(basic_floating) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		if (basic_floating.scalar) {
			emit(words, detail::basic_floating_operand(instruction));
		} else {
			emit(words,
				vector_operand_offset(instruction & 31U) |
				(static_cast<std::uintptr_t>(vector_operand_offset(
					(instruction >> 5) & 31U)) << 16) |
				(static_cast<std::uintptr_t>(vector_operand_offset(
					(instruction >> 16) & 31U)) << 32));
		}
	} else if ((instruction & 0xff20'7c00U) == 0x1e20'4000U ||
		   (instruction & 0xff20'0c00U) == 0x1e20'0800U) {
		const auto type = (instruction >> 22) & 3U;
		const bool one_source =
			(instruction & 0xff20'7c00U) == 0x1e20'4000U;
		const auto operation = one_source ?
			(instruction >> 15) & 0x3fU :
			(instruction >> 12) & 0xfU;
		if (type == 2U)
			return illegal_instruction(context);
		const bool native =
			(type <= 1U && (one_source ? operation <= 3U :
				operation <= 3U || operation == 8U)) ||
			(type == 3U && one_source &&
				(operation <= 2U));
		if (native) {
			emit(words, floating_arithmetic_gadget(
				type, one_source, operation));
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16));
		} else {
			return implementation_gap(context);
		}
	} else if ((instruction & 0xff00'0000U) == 0x1f00'0000U &&
		   ((instruction >> 22) & 3U) <= 1) {
		emit(words, floating_multiply_add_gadget(
			((instruction >> 22) & 3U) == 1,
			(instruction & 0x0020'0000U) != 0,
			(instruction & 0x0000'8000U) != 0));
		emit(words, (instruction & 31U) |
			(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
			(static_cast<std::uintptr_t>((instruction >> 10) & 31U) << 16) |
			(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 24));
	} else if ((instruction & 0xbf20'fc00U) == 0x0e20'e000U) {
		const auto gadget = simd_pmull_gadget(
			(instruction & 0x4000'0000U) != 0,
			(instruction >> 22) & 3U,
			options.force_software_pmull);
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		const auto destination = vector_operand_offset(instruction & 31U);
		const auto left = vector_operand_offset((instruction >> 5) & 31U);
		const auto right =
			vector_operand_offset((instruction >> 16) & 31U);
		emit(words, destination | (left << 16) | (right << 32));
	} else if ((instruction & 0xffe0'8000U) == 0xce00'0000U ||
		   (instruction & 0xffe0'8000U) == 0xce20'0000U) {
		const bool native = !options.force_software_sha3 &&
			host_capabilities().has(ArmFeature::sha3);
		const bool bit_clear =
			(instruction & 0x0020'0000U) != 0;
		emit(words, bit_clear ?
			(native ? uml_arm64_gadget_simd_bcax_native :
			 uml_arm64_gadget_simd_bcax_soft) :
			(native ? uml_arm64_gadget_simd_eor3_native :
			 uml_arm64_gadget_simd_eor3_soft));
		emit(words, simd_quaternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) & 31U, (instruction >> 10) & 31U));
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'8c00U) {
		emit(words, !options.force_software_sha3 &&
				host_capabilities().has(ArmFeature::sha3) ?
			uml_arm64_gadget_simd_rax1_native :
			uml_arm64_gadget_simd_rax1_soft);
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) & 31U));
	} else if ((instruction & 0xffe0'0000U) == 0xce80'0000U) {
		emit(words, simd_xar_gadget(
			(instruction >> 10) & 63U,
			!options.force_software_sha3 &&
				host_capabilities().has(ArmFeature::sha3)));
		emit(words, simd_ternary_operand(
			instruction & 31U, (instruction >> 5) & 31U,
			(instruction >> 16) & 31U));
	} else if ((instruction & 0xffff'8c00U) == 0x4e28'0800U) {
		const auto gadget = simd_aes_gadget(
			(instruction >> 12) & 7U,
			options.force_software_aes);
		if (gadget == nullptr) {
			return illegal_instruction(context);
                        }
                        emit(words, gadget);
                        emit(words, simd_unary_operand(
                                        instruction & 31U,
                                        (instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'8c00U) == 0x5e00'0000U) {
                  const auto gadget = simd_sha_three_gadget(
                      (instruction >> 12) & 7U,
                      options.force_software_fixed_transforms);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if ((instruction & 0xffff'cc00U) == 0x5e28'0800U) {
                  const auto gadget = simd_sha_two_gadget(
                      (instruction >> 12) & 3U,
                      options.force_software_fixed_transforms);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'fc00U) == 0xce60'8000U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'8400U ||
                           (instruction & 0xffe0'fc00U) == 0xce60'8800U) {
                  emit(words, simd_sha512_three_gadget(
                                  instruction,
                                  options.force_software_fixed_transforms));
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if ((instruction & 0xffff'fc00U) == 0xcec0'8000U) {
                  emit(words, !options.force_software_fixed_transforms &&
                                      host_capabilities().has(ArmFeature::sha512)
                                  ? uml_arm64_gadget_simd_sha512su0
                                  : uml_arm64_gadget_simd_sha512su0_soft);
                  emit(words, simd_unary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U));
                } else if ((instruction & 0xffe0'f000U) == 0xce60'c000U ||
                           (instruction & 0xffe0'8000U) == 0xce40'0000U ||
                           (instruction & 0xffe0'c000U) == 0xce40'8000U ||
                           (instruction & 0xffe0'8000U) == 0xce60'0000U ||
                           (instruction & 0xffff'fc00U) == 0xcec0'8400U ||
                           (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
                           (instruction & 0xffff'fc00U) == 0xcec0'8c00U) {
                  /* SM3PARTW1, SM3PARTW2, SM4EKEY, SM3SS1, SM3TT*, and
                   * SM4E need FEAT_SM3 and FEAT_SM4, which the virtual CPU
                   * does not implement; the other cells are reserved. */
                  return illegal_instruction(context);
                } else if ((instruction & 0xbfe0'fc00U) == 0x0e40'1c00U ||
                           (instruction & 0xffe0'fc00U) == 0x5e40'1c00U ||
                           (instruction & 0xbfa0'fc00U) == 0x0e20'dc00U ||
                           (instruction & 0xffa0'fc00U) == 0x5e20'dc00U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const bool half =
                      (instruction & 0x00e0'0000U) == 0x0040'0000U;
                  const auto gadget = simd_fmulx_gadget(
                      scalar, q, half,
                      !half && (instruction & 0x0040'0000U) != 0);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>((instruction >> 16) &
                                                       31U)
                           << 16));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e00'9400U ||
                           (instruction & 0xbf20'fc00U) == 0x0e00'9c00U ||
                           (instruction & 0xdf20'fc00U) == 0x5e00'9400U ||
                           (instruction & 0xff20'fc00U) == 0x5e00'9c00U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0U;
                  const bool ordinary =
                      (instruction & 0x9f20'fc00U) == 0x0e00'9400U;
                  const auto signedness =
                      ordinary ? ((instruction & 0x2000'0000U) != 0
                                      ? SimdDotSignedness::unsigned_unsigned
                                      : SimdDotSignedness::signed_signed)
                               : SimdDotSignedness::unsigned_signed;
                  const auto gadget =
                      !scalar && ((instruction >> 22) & 3U) == 2U
                          ? simd_dot_vector_gadget(
                                signedness, (instruction & 0x4000'0000U) != 0,
                                ordinary ? options.force_software_dotprod
                                         : options.force_software_i8mm)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if ((instruction & 0x9f20'f400U) == 0x0e00'a400U) {
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
                  const unsigned size = (instruction >> 22) & 3U;
                  const bool alternate = (instruction & 0x800U) != 0;
                  if (!q || size != 2U || (unsigned_group && alternate)) {
                    return illegal_instruction(context);
                  }
                  const bool mixed = alternate;
                  const auto signedness =
                      mixed            ? SimdDotSignedness::unsigned_signed
                      : unsigned_group ? SimdDotSignedness::unsigned_unsigned
                                       : SimdDotSignedness::signed_signed;
                  emit(words, simd_matrix_gadget(signedness,
                                                 options.force_software_i8mm));
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if ((instruction & 0xbf20'f400U) == 0x2e00'8400U ||
                           (instruction & 0xff20'f400U) == 0x7e00'8400U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_rdm_three_same_gadget(
                      (instruction & 0x800U) != 0, scalar,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  if (!host_capabilities().has(ArmFeature::rdm)) {
                    emit(words, uml_arm64_gadget_simd_rdm_software);
                    emit(words, instruction);
                    emit(words, address);
                  } else {
                    emit(words, gadget);
                    emit(words, simd_ternary_operand(
                                    instruction & 31U,
                                    (instruction >> 5) & 31U,
                                    (instruction >> 16) & 31U));
                  }
                } else if (const auto floating_conversion =
                               detail::decode_simd_floating_precision_vector(
                                   instruction);
                           floating_conversion.recognized) {
                  if (!floating_conversion.valid) {
                    return illegal_instruction(context);
                  }
                  emit(words, simd_floating_precision_vector_gadget(
                                  floating_conversion));
                  emit(words, detail::simd_floating_precision_vector_operand(
                                  instruction, floating_conversion));
                } else if (const auto floating_compare =
                               detail::decode_simd_floating_compare(
                                   instruction);
                           floating_compare.recognized) {
                  if (!floating_compare.valid) {
                    return illegal_instruction(context);
                  }
                  emit(words, simd_floating_compare_gadget(floating_compare));
                  emit(words, detail::simd_floating_compare_operand(
                                  instruction, floating_compare));
                } else if ((((instruction & 0x9f20'0400U) == 0x0e20'0400U) ||
                            ((instruction & 0xdf20'0400U) == 0x5e20'0400U)) &&
                           ((instruction >> 11) & 0x1fU) < 24U &&
                           ((instruction >> 11) & 0x1fU) != 3U) {
                  /* Integer three-same operations are fully validated and
                   * selected while translating. Their native gadgets contain no
                   * opcode or arrangement branches on the valid hot path. */
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_three_same_gadget(
                      (instruction >> 11) & 0x1fU,
                      (instruction & 0x2000'0000U) != 0, scalar,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  if (!emit_resident_mla_run(context)) {
                    emit(words, gadget);
                    emit(words, vector_operand_offset(instruction & 31U) |
                                    (static_cast<std::uintptr_t>(
                                         vector_operand_offset((instruction >> 5) & 31U))
                                     << 16) |
                                    (static_cast<std::uintptr_t>(
                                         vector_operand_offset((instruction >> 16) & 31U))
                                     << 32));
                  }
                } else if (((instruction & 0xbf20'fc00U) == 0x0e00'1800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'2800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'3800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'5800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'6800U ||
                            (instruction & 0xbf20'fc00U) == 0x0e00'7800U)) {
                  const auto gadget =
                      simd_permute_gadget((instruction >> 12) & 7U,
                                          (instruction & 0x4000'0000U) != 0,
                                          (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, vector_operand_offset(instruction & 31U) |
                                  (static_cast<std::uintptr_t>(
                                       vector_operand_offset((instruction >> 5) & 31U))
                                   << 16) |
                                  (static_cast<std::uintptr_t>(
                                       vector_operand_offset((instruction >> 16) & 31U))
                                   << 32));
                } else if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
                           (instruction & 0x00c0'0000U) != 0U) {
                  /* LUTI2 and LUTI4 need FEAT_LUT, which the virtual CPU
                   * does not implement. */
                  return illegal_instruction(context);
                } else if ((instruction & 0xbfe0'8c00U) == 0x0e00'0000U) {
                  const unsigned count = ((instruction >> 13) & 3U) + 1U;
                  emit(words, simd_table_lookup_gadget(
                                  (instruction & 0x1000U) != 0, count,
                                  (instruction & 0x4000'0000U) != 0));
                  const unsigned first = (instruction >> 5) & 31U;
                  emit(words,
                       vector_operand_offset(instruction & 31U) |
                           (static_cast<std::uintptr_t>(
                                vector_operand_offset((instruction >> 16) & 31U))
                            << 16) |
                           (static_cast<std::uintptr_t>(vector_operand_offset(first))
                            << 32) |
                           (static_cast<std::uintptr_t>(
                                vector_operand_offset((first + 1U) & 31U))
                            << 48));
                  if (count > 2U)
                    emit(words, vector_operand_offset((first + 2U) & 31U) |
                                    (static_cast<std::uintptr_t>(
                                         vector_operand_offset((first + 3U) & 31U))
                                     << 16));
                } else if ((instruction & 0x9ff8'0400U) == 0x0f00'0400U) {
                  /* This allocation overlaps the broad shift-immediate masks
                   * below in cmode bits, so it must be selected first. */
                  const auto decoded =
                      detail::decode_simd_modified_immediate(instruction);
                  if (decoded) {
                    emit(words, simd_modified_immediate_gadget(
                                    decoded->action,
                                    (instruction & 0x4000'0000U) != 0));
                    emit(words, instruction & 31U);
                    emit(words, decoded->pattern);
                  } else {
                    return illegal_instruction(context);
                  }
                } else if ((instruction & 0x9f80'fc00U) == 0x0f00'a400U) {
                  const auto encoded = (instruction >> 16) & 0x7fU;
                  const auto source_bits = std::bit_floor(encoded);
                  const auto gadget = simd_shift_left_long_gadget(
                      source_bits, (instruction & 0x4000'0000U) != 0,
                      (instruction & 0x2000'0000U) != 0);
                  if (gadget == nullptr ||
                      encoded - source_bits >= source_bits) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(encoded - source_bits)
                           << 16));
                } else if ((((instruction & 0x9f20'0c00U) == 0x0e20'0000U) ||
                            ((instruction & 0xdf20'0c00U) == 0x5e20'0000U)) &&
                           ((instruction >> 12) & 15U) != 4U &&
                           ((instruction >> 12) & 15U) != 6U) {
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget = simd_widening_gadget(
                      (instruction >> 12) & 15U,
                      (instruction & 0x2000'0000U) != 0, scalar,
                      !scalar && (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if ((instruction & 0xbfff'fc00U) == 0x0ef8'f800U ||
                           (instruction & 0xbfff'fc00U) == 0x2ef8'f800U ||
                           (instruction & 0xbf3f'fc00U) == 0x0e20'f800U ||
                           (instruction & 0xbf3f'fc00U) == 0x2e20'f800U) {
                  const auto half_operation = instruction & 0xbfff'fc00U;
                  const bool half = half_operation == 0x0ef8'f800U ||
                                    half_operation == 0x2ef8'f800U;
                  const bool negate =
                      half ? half_operation == 0x2ef8'f800U
                           : (instruction & 0xbf3f'fc00U) == 0x2e20'f800U;
                  const bool allocated =
                      half || (instruction & 0x0080'0000U) != 0U;
                  const auto gadget =
                      allocated
                          ? simd_floating_sign_gadget(
                                negate, half,
                                !half && (instruction & 0x0040'0000U) != 0U,
                                (instruction & 0x4000'0000U) != 0U)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0xbfe0'fc00U) == 0x0e00'0400U ||
                           (instruction & 0xffe0'fc00U) == 0x5e00'0400U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_duplicate_element_gadget(
                                 (instruction & 0x1000'0000U) != 0,
                                 (instruction & 0x4000'0000U) != 0,
                                 lane->element_shift)
                           : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0x9f00'0400U) == 0x0f00'0000U &&
                           (((instruction >> 12) & 15U) == 14U ||
                            (((instruction >> 12) & 15U) == 15U &&
                             (instruction & 0x2000'0000U) == 0 &&
                             (((instruction >> 22) & 3U) == 0U ||
                              ((instruction >> 22) & 3U) == 2U)))) {
                  const unsigned operation = (instruction >> 12) & 15U;
                  const unsigned size = (instruction >> 22) & 3U;
                  const auto element =
                      decode_simd_dot_indexed_element(instruction);
                  const auto signedness =
                      operation == 14U
                          ? ((instruction & 0x2000'0000U) != 0
                                 ? SimdDotSignedness::unsigned_unsigned
                                 : SimdDotSignedness::signed_signed)
                      : size == 0U ? SimdDotSignedness::signed_unsigned
                                   : SimdDotSignedness::unsigned_signed;
                  const auto gadget =
                      (operation != 14U || size == 2U)
                          ? simd_dot_indexed_gadget(
                                signedness, (instruction & 0x4000'0000U) != 0,
                                element.lane_index,
                                operation == 14U
                                    ? options.force_software_dotprod
                                    : options.force_software_i8mm)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  element.register_index));
                } else if (((instruction & 0x9f00'0400U) == 0x0f00'0000U ||
                            (instruction & 0xdf00'0400U) == 0x5f00'0000U) &&
                           ((((instruction >> 12) & 15U) == 1U &&
                             (instruction & 0x2000'0000U) == 0) ||
                            (((instruction >> 12) & 15U) == 5U &&
                             (instruction & 0x2000'0000U) == 0) ||
                            ((instruction >> 12) & 15U) == 9U)) {
                  /* All decode choices are fixed here; the gadget is one
                   * branch-free native instruction. */
                  const auto element =
                      decode_simd_indexed_floating_element(instruction);
                  const auto gadget =
                      element
                          ? simd_indexed_floating_gadget(
                                (instruction >> 12) & 15U,
                                (instruction & 0x2000'0000U) != 0,
                                (instruction & 0x1000'0000U) != 0,
                                (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U, element->lane_index)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, vector_operand_offset(instruction & 31U) |
                                  (static_cast<std::uintptr_t>(
                                       vector_operand_offset((instruction >> 5) & 31U))
                                   << 16) |
                                  (static_cast<std::uintptr_t>(
                                       vector_operand_offset(element->register_index))
                                   << 32));
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0000U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0000U)) &&
                           is_simd_indexed_integer_group(
                               (instruction >> 12) & 15U,
                               (instruction & 0x2000'0000U) != 0,
                               (instruction & 0x1000'0000U) != 0)) {
                  /* Integer by-element choices, including the lane and
                   * restricted element-register field, are resolved once while
                   * translating. */
                  const auto element = decode_simd_indexed_element(instruction);
                  const bool rdm = (instruction & 0x2000'0000U) != 0 &&
                                   (((instruction >> 12) & 15U) == 13U ||
                                    ((instruction >> 12) & 15U) == 15U);
                  const auto gadget =
                      element
                          ? simd_indexed_integer_gadget(
                                (instruction >> 12) & 15U,
                                (instruction & 0x2000'0000U) != 0,
                                (instruction & 0x1000'0000U) != 0,
                                (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U, element->lane_index)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  if (rdm && !host_capabilities().has(ArmFeature::rdm)) {
                    emit(words, uml_arm64_gadget_simd_rdm_software);
                    emit(words, instruction);
                    emit(words, address);
                  } else {
                    emit(words, gadget);
                    emit(words, simd_ternary_operand(
                                    instruction & 31U,
                                    (instruction >> 5) & 31U,
                                    element->register_index));
                  }
                } else if ((instruction & 0xdf00'0400U) == 0x5f00'0000U &&
                           is_simd_indexed_scalar_reserved(
                               (instruction >> 12) & 15U,
                               (instruction & 0x2000'0000U) != 0,
                               (instruction >> 22) & 3U)) {
                  /* Reject scalar indexed holes at translation time, before any
                   * architectural register or FP state is observed. */
                  return illegal_instruction(context);
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0400U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0400U)) &&
                           ((((instruction >> 11) & 31U) <= 8U) &&
                            (((instruction >> 11) & 1U) == 0U))) {
                  const auto decoded =
                      decode_simd_immediate_shift(instruction, false);
                  const unsigned opcode = (instruction >> 11) & 31U;
                  const auto gadget =
                      decoded ? simd_shift_right_immediate_gadget(
                                    opcode, (instruction & 0x2000'0000U) != 0,
                                    decoded->shape)
                              : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(decoded->shift) << 16));
                  if (opcode == 8U)
                    emit(words, simd_shift_insert_mask(decoded->lane_bits,
                                                       decoded->shift, false));
                } else if ((instruction & 0x9f00'fc00U) == 0x0f00'5400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'5400U) {
                  const auto decoded =
                      decode_simd_immediate_shift(instruction, true);
                  const bool insert = (instruction & 0x2000'0000U) != 0;
                  const auto gadget =
                      decoded ? simd_shift_left_immediate_gadget(insert,
                                                                 decoded->shape)
                              : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(decoded->shift) << 16));
                  if (insert)
                    emit(words, simd_shift_insert_mask(decoded->lane_bits,
                                                       decoded->shift, true));
                } else if ((instruction & 0x9f00'fc00U) == 0x0f00'7400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'7400U ||
                           (instruction & 0x9f00'fc00U) == 0x0f00'6400U ||
                           (instruction & 0xdf00'fc00U) == 0x5f00'6400U) {
                  const auto decoded =
                      decode_simd_saturating_shift_left(instruction);
                  const bool signed_to_unsigned =
                      ((instruction >> 11) & 31U) == 12U;
                  const bool valid =
                      !signed_to_unsigned || (instruction & 0x2000'0000U) != 0;
                  const auto operation =
                      signed_to_unsigned
                          ? SimdSaturatingShiftLeft::signed_to_unsigned
                      : (instruction & 0x2000'0000U) != 0
                          ? SimdSaturatingShiftLeft::unsigned_to_unsigned
                          : SimdSaturatingShiftLeft::signed_to_signed;
                  const auto gadget =
                      decoded && valid
                          ? simd_saturating_shift_left_gadget(
                                operation, decoded->shape, decoded->shift)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (signed_to_unsigned
                               ? 0U
                               : static_cast<std::uintptr_t>(decoded->shift)
                                     << 16));
                } else if ((((instruction & 0x9f00'0400U) == 0x0f00'0400U) ||
                            ((instruction & 0xdf00'0400U) == 0x5f00'0400U)) &&
                           ((instruction >> 11) & 31U) >= 16U &&
                           ((instruction >> 11) & 31U) <= 19U &&
                           !(((instruction >> 11) & 31U) <= 17U &&
                             (instruction & 0x2000'0000U) == 0 &&
                             (instruction & 0x1000'0000U) == 0)) {
                  const unsigned opcode = (instruction >> 11) & 31U;
                  const unsigned encoded = (instruction >> 16) & 0x7fU;
                  const unsigned source_bits =
                      encoded >= 32U && encoded < 64U   ? 64U
                      : encoded >= 16U && encoded < 32U ? 32U
                      : encoded >= 8U && encoded < 16U  ? 16U
                                                        : 0U;
                  const unsigned shift = source_bits - encoded;
                  const bool scalar = (instruction & 0x1000'0000U) != 0;
                  const auto gadget =
                      shift != 0U && shift <= source_bits / 2U
                          ? simd_saturating_narrow_gadget(
                                opcode, (instruction & 0x2000'0000U) != 0,
                                source_bits, scalar,
                                !scalar && (instruction & 0x4000'0000U) != 0)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(shift) << 16));
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
                  const auto operation =
                      ((instruction >> 22) & 3U) | ((instruction >> 27) & 4U);
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const unsigned destination = instruction & 31U;
                  const unsigned left = (instruction >> 5) & 31U;
                  const unsigned right = (instruction >> 16) & 31U;
                  if (operation == 2U && left == right && q &&
                      destination == left) {
                    emit(words, uml_arm64_gadget_nop);
                  } else if (operation == 2U && left == right) {
                    emit(words, simd_move_gadget(q));
                    emit(words, simd_unary_operand(destination, left));
                  } else {
                    emit(words, simd_bitwise_gadget(operation, q));
                    emit(words, simd_ternary_operand(
                                    destination, left, right));
                  }
                } else if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
                  const bool q = (instruction & 0x4000'0000U) != 0;
                  const unsigned offset = (instruction >> 11) & 15U;
                  if (offset < (q ? 16U : 8U)) {
                    emit(words, simd_extract_gadget(q, offset));
                    emit(words, (instruction & 31U) |
                                    (static_cast<std::uintptr_t>(
                                         (instruction >> 5) & 31U)
                                     << 8) |
                                    (static_cast<std::uintptr_t>(
                                         (instruction >> 16) & 31U)
                                     << 16));
                  } else {
                    return illegal_instruction(context);
                  }
                } else if ((instruction & 0x9f3e'0c00U) == 0x0e30'0800U &&
                           (((instruction >> 12) & 31U) == 3U ||
                            ((instruction >> 12) & 31U) == 10U ||
                            ((instruction >> 12) & 31U) == 26U ||
                            ((instruction >> 12) & 31U) == 27U)) {
                  const auto gadget = simd_integer_reduce_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0x9fbf'fc00U) == 0x0ea1'c800U) {
                  const auto gadget = simd_unsigned_estimate_gadget(
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction & 0x0040'0000U) != 0);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((((instruction & 0x9f3e'0c00U) == 0x0e20'0800U) ||
                            ((instruction & 0xdf3e'0c00U) == 0x5e20'0800U)) &&
                           (((instruction >> 12) & 31U) == 2U ||
                            ((instruction >> 12) & 31U) == 3U ||
                            ((instruction >> 12) & 31U) == 4U ||
                            ((instruction >> 12) & 31U) == 6U ||
                            ((instruction >> 12) & 31U) == 7U ||
                            ((instruction >> 12) & 31U) == 11U ||
                            ((instruction >> 12) & 31U) == 18U ||
                            ((instruction >> 12) & 31U) == 19U ||
                            ((instruction >> 12) & 31U) == 20U)) {
                  const auto gadget = simd_integer_unary_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x1000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0x9f3f'fc00U) == 0x0e20'0800U ||
                           (instruction & 0x9f3f'fc00U) == 0x0e20'1800U) {
                  const bool reverse16 = (instruction & 0x0000'1000U) != 0;
                  const bool alternate = (instruction & 0x2000'0000U) != 0;
                  const unsigned block_bits = reverse16   ? 16U
                                              : alternate ? 32U
                                                          : 64U;
                  const auto gadget =
                      reverse16 && alternate
                          ? nullptr
                          : simd_reverse_native_gadget(
                                block_bits, (instruction & 0x4000'0000U) != 0,
                                (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0x9f3f'fc00U) == 0x0e20'5800U) {
                  const bool alternate = (instruction & 0x2000'0000U) != 0;
                  const unsigned size = (instruction >> 22) & 3U;
                  const unsigned operation = !alternate && size == 0U  ? 0U
                                             : alternate && size == 0U ? 1U
                                             : alternate && size == 1U ? 2U
                                                                       : 3U;
                  if (operation == 3U) {
                    return illegal_instruction(context);
                  }
                  emit(words,
                       simd_byte_unary_gadget(
                           operation, (instruction & 0x4000'0000U) != 0));
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0xdf20'0c00U) == 0x5e20'0800U &&
                           (((instruction >> 12) & 31U) == 0U ||
                            ((instruction >> 12) & 31U) == 1U ||
                            ((instruction >> 12) & 31U) == 5U)) {
                  /* The reverse and byte-unary opcode slots are vector-only. */
                  return illegal_instruction(context);
                } else if ((instruction & 0xffff'fc00U) == 0x5ef1'b800U) {
                  emit(words, uml_arm64_gadget_simd_addp_scalar);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8));
                } else if ((instruction & 0xbf00'fc00U) == 0x0f00'8400U ||
                           (instruction & 0xbf00'fc00U) == 0x0f00'8c00U) {
                  const unsigned encoded = (instruction >> 16) & 0x7fU;
                  const unsigned source_bits =
                      encoded >= 32U && encoded < 64U   ? 64U
                      : encoded >= 16U && encoded < 32U ? 32U
                      : encoded >= 8U && encoded < 16U  ? 16U
                                                        : 0U;
                  const unsigned shift = source_bits - encoded;
                  const auto gadget =
                      shift != 0U && shift <= source_bits / 2U
                          ? simd_shift_narrow_gadget(
                                source_bits, (instruction & 0x4000'0000U) != 0,
                                (instruction & 0x800U) != 0)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(shift) << 16));
                } else if (((instruction & 0xdf00'0400U) == 0x5f00'0400U ||
                            ((instruction & 0x9f00'0400U) == 0x0f00'0400U &&
                             ((instruction >> 16) & 0x7fU) >= 8U)) &&
                           (((instruction >> 11) & 31U) < 20U ||
                            (((instruction >> 11) & 31U) >= 21U &&
                             ((instruction >> 11) & 31U) <= 27U) ||
                            ((instruction >> 11) & 31U) == 29U ||
                            ((instruction >> 11) & 31U) == 30U ||
                            (((instruction & 0x1000'0000U) != 0) &&
                             ((instruction >> 11) & 31U) == 20U))) {
                  /* Every allocated operation in this part of the
                   * immediate-shift class was selected above. Resolve its
                   * reserved holes once during translation and report the
                   * guest-scoped illegal-instruction event directly. */
                  return illegal_instruction(context);
                } else if ((instruction & 0x9f20'fc00U) == 0x0e20'4000U ||
                           (instruction & 0x9f20'fc00U) == 0x0e20'6000U) {
                  const auto gadget = simd_high_narrow_gadget(
                      ((instruction >> 11) & 31U) == 12U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_ternary_operand(
                                  instruction & 31U,
                                  (instruction >> 5) & 31U,
                                  (instruction >> 16) & 31U));
                } else if (((instruction & 0x9f3f'fc00U) == 0x0e20'8800U ||
                            (instruction & 0x9f3f'fc00U) == 0x0e20'9800U ||
                            (instruction & 0x9f3f'fc00U) == 0x0e20'a800U ||
                            (instruction & 0xdf3f'fc00U) == 0x5e20'8800U ||
                            (instruction & 0xdf3f'fc00U) == 0x5e20'9800U ||
                            (instruction & 0xdf3f'fc00U) == 0x5e20'a800U)) {
                  const auto gadget = simd_compare_zero_native_gadget(
                      (instruction >> 12) & 31U,
                      (instruction & 0x2000'0000U) != 0,
                      (instruction & 0x1000'0000U) != 0,
                      (instruction & 0x4000'0000U) != 0,
                      (instruction >> 22) & 3U);
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(words, simd_unary_operand(instruction & 31U,
                                                 (instruction >> 5) & 31U));
                } else if ((instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
                           (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane
                          ? simd_lane_to_gpr_gadget(
                                (instruction & 0x1000U) == 0,
                                (instruction & 0x4000'0000U) != 0,
                                lane->element_shift, (instruction & 31U) == 31U)
                          : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0xffe0'fc00U) == 0x4e00'1c00U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_insert_general_gadget(
                                 lane->element_shift,
                                 ((instruction >> 5) & 31U) == 31U)
                           : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16));
                } else if ((instruction & 0xffe0'8400U) == 0x6e00'0400U) {
                  const auto lane =
                      detail::decode_simd_lane_index((instruction >> 16) & 31U);
                  const auto gadget =
                      lane ? simd_insert_element_gadget(lane->element_shift)
                           : nullptr;
                  if (gadget == nullptr) {
                    return illegal_instruction(context);
                  }
                  const auto source_offset =
                      ((instruction >> 11) & 15U) & ~(lane->element_size - 1U);
                  emit(words, gadget);
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>(lane->offset) << 16) |
                          (static_cast<std::uintptr_t>(source_offset) << 24));
                } else if ((instruction & 0xffff'fc00U) == 0x1ee6'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9ee6'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1ee7'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9ee7'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9e66'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9e67'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1e26'0000U ||
                           (instruction & 0xffff'fc00U) == 0x1e27'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9eae'0000U ||
                           (instruction & 0xffff'fc00U) == 0x9eaf'0000U) {
                  const auto opcode = instruction & 0xffff'fc00U;
                  const bool to_gpr =
                      opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U ||
                      opcode == 0x1e26'0000U || opcode == 0x9e66'0000U ||
                      opcode == 0x9eae'0000U;
                  if (to_gpr && (instruction & 31U) == 31U) {
                    emit(words, uml_arm64_gadget_nop);
                  } else {
                    const bool zero_source = ((instruction >> 5) & 31U) == 31U;
                    Gadget move;
                    if (opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_16_low;
                    else if (opcode == 0x1ee7'0000U || opcode == 0x9ee7'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_16_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_16_low;
                    else if (opcode == 0x9e66'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_64_low;
                    else if (opcode == 0x9e67'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_64_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_64_low;
                    else if (opcode == 0x1e26'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_32_low;
                    else if (opcode == 0x1e27'0000U)
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_32_low_zero
                                 : uml_arm64_gadget_gpr_to_vector_32_low;
                    else if (opcode == 0x9eae'0000U)
                      move = uml_arm64_gadget_vector_to_gpr_64_high;
                    else
                      move = zero_source
                                 ? uml_arm64_gadget_gpr_to_vector_64_high_zero
                                 : uml_arm64_gadget_gpr_to_vector_64_high;
                    emit(words, move);
                    emit(words,
                         (instruction & 31U) | (static_cast<std::uintptr_t>(
                                                    (instruction >> 5) & 31U)
                                                << 8));
                  }
                } else if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U &&
                           ((instruction >> 22) & 3U) != 2U) {
                  emit(words, floating_select_gadget((instruction >> 12) & 15U,
                                                     (instruction >> 22) & 3U));
                  emit(
                      words,
                      (instruction & 31U) |
                          (static_cast<std::uintptr_t>((instruction >> 5) & 31U)
                           << 8) |
                          (static_cast<std::uintptr_t>((instruction >> 16) &
                                                       31U)
                           << 16));
                } else if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U) {
                  return illegal_instruction(context);
	} else if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
		const auto lane = detail::decode_simd_lane_index(
			(instruction >> 16) & 31U);
		const bool q = (instruction & 0x4000'0000U) != 0;
		const auto gadget = lane ? simd_duplicate_general_gadget(q,
			lane->element_shift,
			((instruction >> 5) & 31U) == 31U) : nullptr;
		if (gadget == nullptr)
			return illegal_instruction(context);
		emit(words, gadget);
		emit(words, (instruction & 31U) |
			(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8));
	} else {
		return implementation_gap(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
