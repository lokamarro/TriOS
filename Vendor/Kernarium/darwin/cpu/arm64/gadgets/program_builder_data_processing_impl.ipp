// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

DecodeResult decode_data_processing(DecodeContext &context)
{
	const auto instruction = context.instruction;
	auto &words = context.words;
	const auto &options = context.options;
	const auto &resolve_link = context.resolve_link;

	if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
		const unsigned operation = (instruction >> 29) & 3U;
		const unsigned halfword = (instruction >> 21) & 3U;
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned destination = instruction & 31U;
		if (operation == 1 ||
		    (!is_64_bit && halfword >= 2))
			return illegal_instruction(context);
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else if (operation == 3U) {
			/* MOVK reads and writes Rd, so every one of these is
			 * the in-place shape. */
			emit(words, move_wide_keep_inplace_gadget(
				is_64_bit, halfword, destination));
			emit(words, destination |
				(static_cast<std::uintptr_t>(
					(instruction >> 5) & 0xffffU) << 8));
		} else {
			auto value = static_cast<std::uint64_t>(
				(instruction >> 5) & 0xffffU) << (halfword * 16U);
			if (operation == 0U)
				value = ~value;
			if (!is_64_bit)
				value &= 0xffff'ffffU;
			emit_constant_store(words,
				destination * sizeof(std::uint64_t), value);
		}
	} else if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned source = (instruction >> 5) & 31U;
			const unsigned operation =
				(instruction >> 18) & 3U;
			const unsigned encoded =
				(instruction >> 10) & 0xffU;
			const bool is_64_bit =
				(instruction & 0x8000'0000U) != 0;
			const auto source_offset =
				arithmetic_register_offset(source);
			/* UMAX #0 is an unconditional copy. */
			if (operation == 1U && encoded == 0U) {
				if (is_64_bit && destination == source) {
					emit(words, uml_arm64_gadget_nop);
				} else {
					emit(words, is_64_bit ?
						uml_arm64_gadget_integer_min_max_copy_x :
						uml_arm64_gadget_integer_min_max_copy_w);
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							source_offset)
						 << 16));
				}
			} else {
				std::uint32_t immediate = encoded;
				if ((operation & 1U) == 0U &&
				    (encoded & 0x80U) != 0)
					immediate |= 0xffff'ff00U;
				emit(words, integer_min_max_gadget(
					operation, is_64_bit, true));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						source_offset) << 16) |
					(static_cast<std::uintptr_t>(
						immediate) << 32));
			}
		}
	} else if ((instruction & 0x1f80'0000U) == 0x1180'0000U) {
		/* Includes FEAT_MTE ADDG/SUBG, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
		const bool set_flags = (instruction & 0x2000'0000U) != 0;
		const bool subtract = (instruction & 0x4000'0000U) != 0;
		const bool wide = (instruction & 0x8000'0000U) != 0;
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		const auto immediate = static_cast<std::uintptr_t>(
			(instruction >> 10) & 0xfffU) <<
			((instruction & 0x0040'0000U) != 0 ? 12U : 0U);
		const auto operand = destination |
			(static_cast<std::uintptr_t>(source) << 8) |
			(immediate << 16);
		/* Guest CMP is SUBS with an XZR destination, and it is followed by
		 * B.cond often enough to be worth one entry rather than two. Only
		 * the plain B.cond form is fused: BC.cond shares the top byte but
		 * carries a different hint, and Rn == 31 means SP here, so both fall
		 * through to the unfused pair unchanged. A condition with no fused
		 * entry - the always forms - selects nullptr and does the same. */
		if (set_flags && subtract && destination == 31U &&
		    source != 31U && !context.following.empty() &&
		    (context.following.front() & 0xff00'0010U) == 0x5400'0000U) {
			const auto branch = context.following.front();
			if (const auto fused = compare_immediate_branch_gadget(
				    wide, branch & 15U, resolve_link != nullptr);
			    fused != nullptr) {
				return emit_fused_compare_branch(
					context, branch, fused,
					resolve_link ?
					compare_immediate_branch_inline_gadget(
						wide, branch & 15U) : nullptr,
					operand, 40);
			}
		}
		/* Rd == Rn and neither is the SP/XZR encoding: the entry can
		 * name one CpuState displacement for both accesses. The operand
		 * word is unchanged, so the two now-dead register fields cost
		 * nothing and the fused and SP paths above are untouched. */
		if (destination == source && destination != 31U) {
			emit(words, add_sub_immediate_inplace_gadget(
				wide, subtract, set_flags, destination));
			emit(words, operand);
		} else
		{
			emit(words, add_sub_immediate_gadget(
				wide, subtract, set_flags,
				source == 31U, destination == 31U));
			emit(words, operand);
		}
	} else if ((instruction & 0x1f20'0000U) == 0x0b00'0000U ||
		   (instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
		const bool extended = (instruction & 0x0020'0000U) != 0;
		const unsigned shift = (instruction >> 10) &
			(extended ? 0x7U : 0x3fU);
		const unsigned shift_type = (instruction >> 22) & 0x3U;
		const bool is_64_bit = (instruction >> 31) != 0;
		if ((extended && shift > 4) ||
		    (!extended && (shift_type == 3 ||
			(!is_64_bit && shift >= 32)))) {
			return illegal_instruction(context);
		}
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const bool set_flags = (instruction & 0x2000'0000U) != 0;
		const bool left_zero = !extended && left == 31U;
		const bool right_zero = right == 31U;
		const bool destination_discard = destination == 31U &&
			(!extended || set_flags);
		const unsigned destination_offset = destination == 31U ?
			offsetof(CpuState, sp) : destination * sizeof(std::uint64_t);
		const unsigned left_offset = left == 31U ?
			offsetof(CpuState, sp) : left * sizeof(std::uint64_t);
		const unsigned right_offset = right_zero ? 0U :
			right * sizeof(std::uint64_t);
		const unsigned transform = extended ? 0U :
			shift == 0U ? 0U : shift_type + 1U;
		const auto operand = destination_offset |
			(static_cast<std::uintptr_t>(left_offset) << 16) |
			(static_cast<std::uintptr_t>(right_offset) << 32) |
			(static_cast<std::uintptr_t>(shift) << 48);
		/* Guest CMP of two registers followed by B.cond, which G116's pair
		 * profile ranks far above the immediate form G111 fused. Same
		 * mechanism: the dispatch between the two entries goes, and with it
		 * the msr that reloads the flags the compare just wrote.
		 *
		 * Only the plain unshifted register-register form with an XZR
		 * destination: an extension, a shift, or a real destination leaves
		 * something the fused entry does not compute, and falls through to
		 * the unfused pair unchanged. */
		if (set_flags && (instruction & 0x4000'0000U) != 0 && !extended &&
		    shift == 0U && destination == 31U && !left_zero &&
		    !right_zero && !context.following.empty() &&
		    (context.following.front() & 0xff00'0010U) == 0x5400'0000U) {
			const auto branch = context.following.front();
			if (const auto fused = compare_register_branch_gadget(
				    is_64_bit, branch & 15U, resolve_link != nullptr);
			    fused != nullptr) {
				return emit_fused_compare_branch(
					context, branch, fused,
					resolve_link ?
					compare_register_branch_inline_gadget(
						is_64_bit, branch & 15U) : nullptr,
					operand, 48);
			}
		}
		/* "add/sub Rd, Rd, Rm" with no shift or extension: the
		 * destination and left source name one CpuState slot, so one
		 * baked displacement serves both while Rm keeps its indexed
		 * load. An XZR operand or an SP/discard destination keeps the
		 * established entry. */
		if (!extended && transform == 0U && destination == left &&
		    destination != 31U && right != 31U) {
			emit(words, add_sub_register_inplace_gadget(is_64_bit,
				(instruction & 0x4000'0000U) != 0, set_flags,
				destination));
			emit(words, operand);
		} else
		{
			emit(words, add_sub_register_gadget(is_64_bit,
				(instruction & 0x4000'0000U) != 0, set_flags, extended,
				transform, (instruction >> 13) & 7U, shift != 0U,
				left_zero, right_zero, destination_discard));
			emit(words, operand);
		}
	} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'1000U ||
		   (instruction & 0xffe0'fc00U) == 0x9ac0'1400U ||
		   (instruction & 0xffe0'fc00U) == 0x9ac0'0000U ||
		   (instruction & 0xffe0'fc00U) == 0xbac0'0000U ||
		   (instruction & 0xffe0'e000U) == 0x9a00'2000U ||
		   (instruction & 0xffe0'e000U) == 0xda00'2000U) {
		/* FEAT_MTE IRG/GMI/SUBP/SUBPS and FEAT_CPA ADDPT/SUBPT: the
		 * virtual CPU implements neither feature. */
		return illegal_instruction(context);
	} else if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const bool set_flags =
			(instruction & 0x2000'0000U) != 0;
		if (destination == 31U && !set_flags) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, add_sub_carry_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction & 0x4000'0000U) != 0,
				set_flags, left == 31U, right == 31U,
				left == right && left != 31U,
				destination == 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0xffe0'7c10U) == 0xba00'0400U) {
		const unsigned source = (instruction >> 5) & 31U;
		const auto shifted_mask =
			static_cast<std::uintptr_t>(instruction & 15U) << 28;
		if (shifted_mask == 0U) {
			emit(words, uml_arm64_gadget_nop);
		} else if (source == 31U) {
			emit(words, uml_arm64_gadget_rmif_zero);
			emit(words, shifted_mask);
		} else {
			emit(words, uml_arm64_gadget_rmif_fast);
			emit(words,
				packed_u16_operands(
					static_cast<std::uintptr_t>(source * 8U),
					static_cast<std::uintptr_t>(
						(instruction >> 15) & 63U)) |
				(shifted_mask << 32));
		}
	} else if ((instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
		const unsigned source = (instruction >> 5) & 31U;
		const auto source_offset = arithmetic_register_offset(source);
		const bool native = !options.force_software_flags &&
			host_capabilities().has(ArmFeature::flag_m);
		if ((instruction & 0x4000U) != 0)
			emit(words, native ? uml_arm64_gadget_setf_native_16 :
				uml_arm64_gadget_setf_fast_16);
		else
			emit(words, native ? uml_arm64_gadget_setf_native_8 :
				uml_arm64_gadget_setf_fast_8);
		emit(words, source_offset);
	} else if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
		   (instruction & 0xffff'0800U) == 0x3a00'0800U) {
		return illegal_instruction(context);
	} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'3000U) {
		/* PACGA needs FEAT_PAuth, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const unsigned operation =
				(instruction >> 10) & 3U;
			const bool is_64_bit =
				(instruction & 0x8000'0000U) != 0;
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto left_offset = left == 31U ? zero : left * 8U;
			const auto right_offset = right == 31U ? zero : right * 8U;
			std::optional<unsigned> copy_source;
			if (left == right)
				copy_source = left;
			else if (operation == 1U) {
				if (left == 31U)
					copy_source = right;
				else if (right == 31U)
					copy_source = left;
			}
			else if (operation == 3U &&
				 (left == 31U || right == 31U))
				copy_source = 31U;
			if (copy_source) {
				const auto source_offset =
					*copy_source == 31U ? zero :
						*copy_source * 8U;
				if (is_64_bit &&
				    destination == *copy_source) {
					emit(words,
						uml_arm64_gadget_nop);
				} else {
					emit(words, is_64_bit ?
						uml_arm64_gadget_integer_min_max_copy_x :
						uml_arm64_gadget_integer_min_max_copy_w);
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							source_offset)
						 << 16));
				}
			} else {
				emit(words, integer_min_max_gadget(
					operation, is_64_bit, false));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						left_offset) << 16) |
					(static_cast<std::uintptr_t>(
						right_offset) << 32));
			}
		}
	} else if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U) {
		return illegal_instruction(context);
	} else if ((instruction & 0xffe0'0000U) == 0x9b60'0000U) {
		/* FEAT_CPA MADDPT/MSUBPT are not implemented. */
		return illegal_instruction(context);
	} else if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto operation = (instruction >> 21) & 7U;
		const bool valid = ((instruction >> 29) & 3U) == 0 &&
			(operation == 0 || (is_64_bit && (operation == 1 ||
			 operation == 2 || operation == 5 || operation == 6))) &&
			((instruction & 0x0000'8000U) == 0 ||
			 operation == 0 || operation == 1 || operation == 5) &&
			((operation != 2 && operation != 6) ||
			 ((instruction >> 10) & 0x1fU) == 0x1fU);
		if (valid) {
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned left =
					(instruction >> 5) & 31U;
				const unsigned addend =
					(instruction >> 10) & 31U;
				const unsigned right =
					(instruction >> 16) & 31U;
				emit(words, multiply_add_gadget(operation,
					(instruction & 0x8000U) != 0,
					is_64_bit, left == 31U,
					right == 31U, addend == 31U));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						left * 8U) << 16) |
					(static_cast<std::uintptr_t>(
						right * 8U) << 32) |
					(static_cast<std::uintptr_t>(
						addend * 8U) << 48));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'2000U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, variable_shift_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction >> 10) & 3U,
				left == 31U, right == 31U,
				left == right && left != 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0x7fe0'f800U) == 0x1ac0'0800U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, divide_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction & 0x400U) != 0,
				left == 31U, right == 31U,
				left == right && left != 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
		const unsigned size = (instruction >> 10) & 3U;
		if (((instruction >> 31) != 0) == (size == 3U)) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, crc32_gadget(
					(instruction >> 10) & 7U,
					left == 31U, right == 31U,
					left == right && left != 31U,
					!options.force_software_checksum &&
						host_capabilities().crc32));
				emit(words, arithmetic_binary_operand(instruction));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
		/* Every allocated data-processing (2 source) member is selected
		 * above. The remaining opcode combinations are reserved.
		 */
		return illegal_instruction(context);
	} else if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned shift = (instruction >> 10) & 0x3fU;
		if (((instruction >> 29) & 3U) == 0U &&
		    (instruction & 0x0020'0000U) == 0U &&
		    ((instruction >> 22) & 1U) ==
			    static_cast<unsigned>(is_64_bit) &&
		    (is_64_bit || shift < 32U)) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			emit(words, extract_register_gadget(is_64_bit,
				shift != 0U, left == 31U, right == 31U,
				destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(left) << 8) |
				(static_cast<std::uintptr_t>(right) << 16) |
				(static_cast<std::uintptr_t>(shift) << 24));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned shift = (instruction >> 10) & 0x3fU;
		if (!is_64_bit && shift >= 32)
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const unsigned operation = (instruction >> 29) & 3U;
		if (destination == 31U && operation != 3U) {
			/* A non-flag-setting write to XZR/WZR has no effect. */
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned shift_kind = shift == 0U ? 0U :
				((instruction >> 22) & 3U) + 1U;
			emit(words, logical_shifted_gadget(is_64_bit,
				operation,
				(instruction & 0x0020'0000U) != 0,
				shift_kind, left == 31U, right == 31U,
				destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(left) << 8) |
				(static_cast<std::uintptr_t>(right) << 16) |
				(static_cast<std::uintptr_t>(shift) << 24));
		}
	} else if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
		if ((instruction & 0x0000'0800U) != 0)
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const unsigned operation = ((instruction >> 29) & 2U) |
				((instruction >> 10) & 1U);
			const unsigned condition =
				(instruction >> 12) & 15U;
			const bool unconditional_copy =
				condition >= 14U ||
				(operation == 0U && left == right);
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto left_offset = left == 31U ? zero : left * 8U;
			const auto right_offset = right == 31U ? zero : right * 8U;
			emit(words, conditional_select_gadget(
				unconditional_copy ? 14U : condition,
				operation,
				(instruction & 0x8000'0000U) != 0));
			emit(words,
				static_cast<std::uintptr_t>(destination * 8U) |
				(static_cast<std::uintptr_t>(left_offset) << 16) |
				(static_cast<std::uintptr_t>(
					unconditional_copy ? 0U :
						right_offset)
				 << 32));
		}
	} else if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
		if ((instruction & 0x0000'0410U) != 0)
			return illegal_instruction(context);
		const bool immediate = (instruction & 0x800U) != 0;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const auto left_operand = arithmetic_register_offset(left);
		const auto right_operand = immediate ? right :
			arithmetic_register_offset(right);
		emit(words, conditional_compare_gadget(
			(instruction >> 12) & 15U,
			(instruction & 0x4000'0000U) != 0,
			(instruction & 0x8000'0000U) != 0, immediate));
		emit(words, static_cast<std::uintptr_t>(left_operand) |
			(static_cast<std::uintptr_t>(right_operand) << 16) |
			(static_cast<std::uintptr_t>(instruction & 15U) << 60));
	} else if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto immediate = detail::decode_logical_immediate(
			is_64_bit, (instruction >> 22) & 1U,
			(instruction >> 16) & 0x3fU,
			(instruction >> 10) & 0x3fU);
		if (!immediate)
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		if (destination == source && destination != 31U) {
			/* The baked entry reads no register field: the immediate
			 * is its only operand. */
			emit(words, logical_immediate_inplace_gadget(is_64_bit,
				(instruction >> 29) & 3U, destination));
			emit(words, static_cast<std::uintptr_t>(*immediate));
		} else {
			emit(words, logical_immediate_gadget(is_64_bit,
				(instruction >> 29) & 3U, source == 31U,
				destination == 31U));
			const auto registers = destination |
				(static_cast<std::uintptr_t>(source) << 8);
			/* A 32-bit immediate shares the register word. */
			if (is_64_bit) {
				emit(words, registers);
				emit(words, static_cast<std::uintptr_t>(*immediate));
			} else {
				emit(words, registers |
					(static_cast<std::uintptr_t>(*immediate)
					 << 32));
			}
		}
	} else if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto n = (instruction >> 22) & 1U;
		const auto rotate = (instruction >> 16) & 0x3fU;
		const auto imms = (instruction >> 10) & 0x3fU;
		const auto operation = (instruction >> 29) & 3U;
		const auto masks = detail::decode_bit_masks(
			is_64_bit, n, rotate, imms, false);
		if (operation == 3 || n != static_cast<unsigned>(is_64_bit) ||
		    (!is_64_bit && ((rotate | imms) & 0x20U) != 0) ||
		    !masks)
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned source = (instruction >> 5) & 31U;
			if (operation == 1U) {
				emit(words, bitfield_gadget(is_64_bit, operation,
					rotate != 0U, source == 31U));
				emit(words,
					static_cast<std::uintptr_t>(destination * 8U) |
					(static_cast<std::uintptr_t>(source * 8U) << 16) |
					(static_cast<std::uintptr_t>(rotate) << 32) |
					(static_cast<std::uintptr_t>(imms) << 40));
				emit(words, masks->write);
				emit(words, masks->test);
			} else {
				/* SBFM and UBFM are (source << left) >> right;
				 * engine/bitfield_shift.inc derives the amounts
				 * and says where the operand keeps them. */
				const unsigned mask = is_64_bit ? 63U : 31U;
				const unsigned left = mask - imms;
				const unsigned right = (rotate - imms - 1U) & mask;
				emit(words, bitfield_shift_gadget(is_64_bit,
					operation == 0U, left != 0U, right != 0U));
				emit(words, (left != 0U ? left : right) |
					(static_cast<std::uintptr_t>(right) << 6) |
					(static_cast<std::uintptr_t>(destination * 8U) << 16) |
					(arithmetic_register_offset(source) << 32));
			}
		}
	} else if ((instruction & 0xffff'c000U) == 0xdac1'0000U ||
		   (instruction & 0xffff'f800U) == 0xdac1'4000U) {
		/* PAC*, AUT*, XPACI and XPACD need FEAT_PAuth, which the virtual
		 * CPU does not implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto operation = (instruction >> 10) & 0x1fU;
		if (operation > 8 || (!is_64_bit && operation == 3))
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, bit_reverse_count_gadget(
				is_64_bit, operation));
			emit(words, packed_u16_operands(
				arithmetic_register_offset(source),
				offsetof(CpuState, registers) +
					destination * sizeof(std::uint64_t)));
		}
	} else {
		return implementation_gap(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
