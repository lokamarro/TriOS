// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

namespace {

/* ISB: the gadget leaves at the fall-through when another vCPU has changed the
 * address space's code since this program was validated (engine/
 * system_atomic.inc). Leaving from inside the block gives back the
 * instructions translated after it, a suffix `build` fills in as it does for
 * an inlined conditional branch. With no suffix slot left the block ends at
 * the ISB instead, so there is nothing after it to give back. */
[[nodiscard]] DecodeResult emit_context_synchronization(DecodeContext &context)
{
	auto &words = context.words;
	const auto fallthrough = context.address + sizeof(std::uint32_t);
	emit(words, uml_arm64_gadget_isb);
	const bool inline_suffix = !context.suffixes.full();
	if (inline_suffix)
		context.suffixes.add(words.size(), context.compiled + 1);
	words.push_back(0);
	words.push_back(fallthrough);
	if (inline_suffix)
		return DecodeResult::decoded;
	++context.compiled;
	if (context.resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit_link(context, fallthrough);
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		words.push_back(fallthrough);
	}
	return DecodeResult::terminal;
}

} // namespace

DecodeResult decode_branch_system(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &compiled = context.compiled;
	auto &words = context.words;
	const auto &resolve_link = context.resolve_link;
	const auto &resolve_indirect_link = context.resolve_indirect_link;
	const auto &options = context.options;

	if (pointer_auth_lr_unavailable(instruction)) {
		/* FEAT_PAuth_LR is not advertised by the static guest profile.
		 * Its non-HINT encodings are architecturally UNDEFINED. */
		return illegal_instruction(context);
	} else if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
		const auto displacement = sign_extend(
			static_cast<std::uint64_t>(instruction & 0x03ff'ffffU) << 2,
			28);
		const auto target = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(address) + displacement);
		const bool link = (instruction >> 31) != 0;
		if (resolve_link) {
			emit(words, link ? uml_arm64_gadget_branch_link_linked :
				uml_arm64_gadget_branch_linked);
			emit_link(context, target);
		} else {
			emit(words, link ? uml_arm64_gadget_branch_link :
				uml_arm64_gadget_branch);
			emit(words, target);
		}
		/* The linked call also pushes the return block's link, and
		 * reads the return address it sets out of that BlockLink. */
		if (link && resolve_link)
			emit_link(context, address + sizeof(instruction));
		else if (link)
			emit(words, address + sizeof(instruction));
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xff00'0010U) == 0x5400'0010U) {
		/* BC.cond needs FEAT_HBC, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0xff00'0010U) == 0x5400'0000U) {
		const auto target = branch19_target(address, instruction);
		const auto fallthrough = address + sizeof(instruction);
		if (resolve_link) {
			/* Translating the fall-through behind the branch keeps
			 * the block growing past what is otherwise its first and
			 * usually only branch. */
			if (emit_inlined_conditional(
				    context,
				    conditional_branch_inline_gadget(
					    instruction & 15U),
				    target))
				return DecodeResult::decoded;
			emit(words, conditional_branch_linked_gadget(
				instruction & 15U));
			emit_link(context, target);
			emit_link(context, fallthrough);
		} else {
			emit(words, conditional_branch_gadget(instruction & 15U));
			emit(words, target);
			emit(words, fallthrough);
		}
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0x7e00'0000U) == 0x7400'0000U) {
		/* The virtual CPU implements no FEAT_CMPBR: the compare-and-branch
		 * class is unallocated. */
		return illegal_instruction(context);
	} else if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
		const auto target = branch19_target(address, instruction);
		const auto fallthrough = address + sizeof(instruction);
		const unsigned source = instruction & 31U;
		const bool nonzero = (instruction & 0x0100'0000U) != 0;
		if (source == 31U) {
			const auto selected = nonzero ? fallthrough : target;
			if (resolve_link) {
				emit(words, uml_arm64_gadget_branch_always_linked);
				emit_link(context, selected);
			} else {
				emit(words, uml_arm64_gadget_branch_always);
				emit(words, selected);
			}
		} else if (resolve_link &&
			   emit_inlined_compare_zero_branch(
				   context,
				   compare_branch_inline_gadget(
					   (instruction >> 31) != 0, nonzero),
				   offsetof(CpuState, registers) + source * 8U,
				   target)) {
			return DecodeResult::decoded;
		} else {
			emit(words, compare_branch_gadget(
				(instruction >> 31) != 0, nonzero,
				static_cast<bool>(resolve_link)));
			emit(words, offsetof(CpuState, registers) + source * 8U);
			emit_edges(context, target, fallthrough);
		}
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
		const auto displacement = sign_extend(
			static_cast<std::uint64_t>((instruction >> 5) & 0x3fffU) << 2,
			16);
		const auto bit = ((instruction >> 31) << 5) |
			((instruction >> 19) & 31U);
		const auto target = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(address) + displacement);
		const auto fallthrough = address + sizeof(instruction);
		const unsigned source = instruction & 31U;
		const bool nonzero = (instruction & 0x0100'0000U) != 0;
		if (source == 31U) {
			const auto selected = nonzero ? fallthrough : target;
			if (resolve_link) {
				emit(words, uml_arm64_gadget_branch_always_linked);
				emit_link(context, selected);
			} else {
				emit(words, uml_arm64_gadget_branch_always);
				emit(words, selected);
			}
		} else if (resolve_link &&
			   emit_inlined_packed_compare_branch(
				   context, test_bit_branch_inline_gadget(nonzero),
				   bit | ((offsetof(CpuState, registers) +
					   source * 8U) << 16),
				   target, 32)) {
			return DecodeResult::decoded;
		} else {
			emit(words, test_bit_branch_gadget(nonzero,
				static_cast<bool>(resolve_link)));
			emit(words, bit | ((offsetof(CpuState, registers) +
				source * 8U) << 16));
			emit_edges(context, target, fallthrough);
		}
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xfedf'f800U) == 0xd61f'0800U ||
		   (instruction & 0xffff'fbffU) == 0xd65f'0bffU) {
		/* BRAA, BRAB, BLRAA, BLRAB, their zero-modifier forms, RETAA and
		 * RETAB need FEAT_PAuth, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
		   (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
		   (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
		const auto opcode = instruction & 0xffff'fc1fU;
		const unsigned source = (instruction >> 5) & 31U;
		/* A cached call also names the return block's link, so it needs
		 * the direct-link resolver too. */
		const bool cached = static_cast<bool>(resolve_indirect_link) &&
			(opcode != 0xd63f'0000U ||
				static_cast<bool>(resolve_link));
		/* RET through X30 names no source register; the entry loads it
		 * from its fixed place. */
		const bool baked_return = cached && opcode == 0xd65f'0000U &&
			source == 30U;
		emit(words, opcode == 0xd61f'0000U ?
			(cached ? uml_arm64_gadget_branch_register_cached :
				uml_arm64_gadget_branch_register) :
			opcode == 0xd63f'0000U ?
			(cached ? uml_arm64_gadget_branch_link_register_cached :
				uml_arm64_gadget_branch_link_register) :
			baked_return ? uml_arm64_gadget_return_x30_cached :
			(cached ? uml_arm64_gadget_return_cached :
				uml_arm64_gadget_return));
		if (!baked_return)
			emit(words, arithmetic_register_offset(source));
		if (cached)
			emit_indirect_link(context, address,
				opcode == 0xd65f'0000U);
		/* As for a direct call, a cached one's return address is the
		 * return block's link's own. */
		if (opcode == 0xd63f'0000U && cached)
			emit_link(context, address + sizeof(instruction));
		else if (opcode == 0xd63f'0000U)
			emit(words, address + sizeof(instruction));
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
		const auto immediate = static_cast<std::uint64_t>(
			((instruction >> 5) & 0x7ffffU) << 2) |
			((instruction >> 29) & 3U);
		const auto relative = sign_extend(immediate, 21);
		const auto value = (instruction >> 31) != 0 ?
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address & ~0xfffULL) +
				relative * 4096) :
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + relative);
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			auto folded = value;
			std::size_t consumed = 1;
			/* ADRP is already folded to a constant here, so the
			 * `adrp xN, sym` / `add xN, xN, #:lo12:sym` idiom that
			 * dominates position-independent code is two entries and two
			 * dispatches computing something translation already knows.
			 * When the ADD rewrites the same register it read, the page
			 * value it consumes is dead the moment it is produced, so the
			 * pair collapses into one constant store of the final address -
			 * no extra gadget and no bank growth.
			 *
			 * G113 found it among the most frequent adjacent pairs in
			 * translated code.
			 *
			 * Restricted to the exact idiom: 64-bit, no flags, no shifted
			 * immediate, and Rd == Rn == the ADRP destination. Anything
			 * else leaves the page value observable and is left alone.
			 * Neither instruction can fault, so nothing can be delivered
			 * between them and retiring both together is sound. */
			if (!context.following.empty()) {
				const auto next = context.following.front();
				const bool add_immediate =
					(next & 0xffc0'0000U) == 0x9100'0000U;
				if (add_immediate && (next & 31U) == destination &&
				    ((next >> 5) & 31U) == destination) {
					folded += (next >> 10) & 0xfffU;
					consumed = 2;
				}
			}
			emit_constant_store(words, destination * 8U, folded);
			/* Account for the folded ADD only; Builder::build() adds the
			 * ADRP itself when a decode returns `decoded`. */
			if (consumed == 2)
				++compiled;
		}
	} else if (instruction == 0xd500'401fU) {
		emit(words, uml_arm64_gadget_cfinv);
	} else if (instruction == 0xd500'405fU) {
		emit(words, !options.force_software_flags &&
				host_capabilities().has(ArmFeature::flag_m2) ?
			uml_arm64_gadget_axflag_native :
			uml_arm64_gadget_axflag);
	} else if (instruction == 0xd500'403fU) {
		emit(words, !options.force_software_flags &&
				host_capabilities().has(ArmFeature::flag_m2) ?
			uml_arm64_gadget_xaflag_native :
			uml_arm64_gadget_xaflag);
	} else if ((instruction & 0xfff8'f01fU) == 0xd500'401fU) {
		const auto selected = pstate_immediate_gadget(instruction);
		if (!selected.valid)
			return decode_sme_pstate(context);
		emit(words, selected.gadget);
		if (selected.fault_address)
			emit(words, address);
	} else if ((instruction & 0xffff'ffe0U) == 0xd53b'd040U ||
		   (instruction & 0xffff'ffe0U) == 0xd51b'd040U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'd060U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'00e0U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'0020U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'4400U ||
		   (instruction & 0xffff'ffe0U) == 0xd51b'4400U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'4420U ||
		   (instruction & 0xffff'ffe0U) == 0xd51b'4420U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'4200U ||
		   (instruction & 0xffff'ffe0U) == 0xd51b'4200U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'e000U ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'e040U ||
		   ((instruction & 0xffff'ffe0U) == 0xd53b'e0c0U &&
		    host_capabilities().has(ArmFeature::ecv)) ||
		   (instruction & 0xffff'ffe0U) == 0xd53b'42a0U ||
		   (instruction & 0xffff'ffe0U) == 0xd51b'42a0U) {
		const auto selected = system_register_gadget(instruction);
		emit(words, selected.gadget);
		if (selected.operand)
			emit(words, static_cast<std::uintptr_t>(
				(instruction & 31U) * sizeof(std::uint64_t)));
		/* A register-sourced FPCR write may enable synchronous exception
		 * reporting. Supply its already-known fallthrough PC so the
		 * rare enabled case can leave the block immediately; ordinary
		 * control-only writes remain straight through.
		 */
		if ((instruction & 0xffff'ffe0U) == 0xd51b'4400U &&
		    (instruction & 31U) != 31U) {
			emit(words, address + sizeof(instruction));
		}
	} else if (arm64::detail::emulated_id_register(instruction)) {
		/* The identification registers Linux emulates for EL0 hold one
		 * value for the whole process, so the read folds to the constant
		 * store the translator already has for MOVZ. */
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit_constant_store(words,
				destination * sizeof(std::uint64_t),
				arm64::detail::emulated_id_register_value(
					instruction));
		}
	} else if ((instruction & 0xffd0'0000U) == 0xd510'0000U ||
		   (instruction & 0xffd0'0000U) == 0xd550'0000U ||
		   (instruction & 0xfff8'0000U) == 0xd548'0000U) {
		/* Every implemented EL0 register is selected above, apart from
		 * SME's. Unknown 64-bit registers plus FEAT_SYSREG128/SYSINSTR128
		 * operations are inaccessible in the current userspace
		 * architectural profile. */
		return decode_sme_system_register(context);
	} else if ((instruction & 0xfff8'0000U) == 0xd508'0000U ||
		   (instruction & 0xfff8'0000U) == 0xd528'0000U) {
		const auto operation = instruction & 0xffff'ffe0U;
		if (operation == 0xd50b'7420U) {
			const unsigned source = instruction & 31U;
			emit(words, source == 31U ?
				uml_arm64_gadget_cache_zero_z :
				uml_arm64_gadget_cache_zero_r);
			if (source != 31U)
				emit(words, static_cast<std::uintptr_t>(
					source * sizeof(std::uint64_t)));
			emit(words, address);
		} else if (operation == 0xd50b'7a20U ||
			   operation == 0xd50b'7b20U ||
			   operation == 0xd50b'7e20U) {
			/* Guest writes already update the coherent backing. */
			emit(words, uml_arm64_gadget_nop);
		} else if (operation == 0xd50b'7520U) {
			const unsigned source = instruction & 31U;
			emit(words, source == 31U ?
				uml_arm64_gadget_code_synchronize_z :
				uml_arm64_gadget_code_synchronize_r);
			if (source != 31U)
				emit(words, static_cast<std::uintptr_t>(
					source * sizeof(std::uint64_t)));
			emit(words, address + sizeof(instruction));
			++compiled;
			return DecodeResult::terminal;
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xffff'f0ffU) == 0xd503'305fU) {
		emit(words, uml_arm64_gadget_clear_exclusive);
	} else if ((instruction & 0xffff'f0ffU) == 0xd503'30dfU) {
		return emit_context_synchronization(context);
	} else if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
		   (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
		   instruction == 0xd503'30ffU) {
		emit(words, memory_barrier_gadget(instruction));
	} else if ((instruction & 0xffff'f0ffU) == 0xd503'30ffU ||
		   (instruction & 0xffff'f3ffU) == 0xd503'323fU) {
		/* SB with a nonzero CRm is reserved, and DSB nXS needs
		 * FEAT_XS, which the virtual CPU does not implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
		emit(words, architectural_hint_gadget(instruction));
	} else if ((instruction & 0xffd8'f000U) == 0xd500'2000U) {
		emit(words, uml_arm64_gadget_nop);
	} else if ((instruction & 0xffff'ffc0U) == 0xd503'1000U ||
		   instruction == 0xd6bf'03e0U ||
		   instruction == 0xd69f'03e0U ||
		   instruction == 0xd69f'0bffU ||
		   instruction == 0xd69f'0fffU) {
		/* WFET and WFIT need FEAT_WFxT, which the virtual CPU does not
		 * implement; DRPS and the ERET forms are not EL0 instructions. */
		return illegal_instruction(context);
	} else {
		return decode_sme_space(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
