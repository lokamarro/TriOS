// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

/* The exclusive and vector pair operands pack their register offsets into
 * 12-bit fields. */
static_assert(vector_operand_offset(31U) <= 0xfffU &&
	      base_register_offset(31U) <= 0xfffU &&
	      transfer_register_offset(31U) <= 0xfffU,
	      "a packed memory operand's register offset must fit its field");

DecodeResult decode_load_store(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &words = context.words;

	if ((instruction & 0xffff'ec00U) == 0xd91f'0c00U) {
		/* FEAT_GCS GCSSTR/GCSSTTR: the virtual CPU does not implement
		 * FEAT_GCS. */
		return illegal_instruction(context);
	}
	if ((instruction & 0xff00'0000U) == 0xd800'0000U) {
		/* PRFM (literal) is an implementation-defined performance hint. */
		emit(words, uml_arm64_gadget_nop);
	} else if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
		const bool vector = (instruction & 0x0400'0000U) != 0;
		const unsigned operation = instruction >> 30;
		if (vector && operation == 3U)
			return illegal_instruction(context);
		const unsigned destination = instruction & 31U;
		const auto displacement =
			sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
		const auto target =
			address + static_cast<std::uint64_t>(displacement);
		/* The target address, the destination and the instruction's
		 * position share one operand when the target fits 48 bits,
		 * which only a PC within a megabyte of either end of the
		 * address space does not. A vector destination is named by
		 * its register's offset in sixteen-byte units. */
		if (target >> 48 == 0U) {
			emit(words, literal_load_gadget(vector, operation,
				!vector && destination == 31U, false));
			const auto destination_field = vector ?
				vector_operand_offset(destination) / 16U :
				transfer_register_offset(destination);
			emit(words, target |
				(static_cast<std::uintptr_t>(destination_field)
					<< 48) |
				fault_position(context));
		} else {
			emit(words, literal_load_gadget(vector, operation,
				!vector && destination == 31U, true));
			const auto destination_offset = vector ?
				vector_operand_offset(destination) :
				transfer_register_offset(destination);
			emit(words, destination_offset |
				(static_cast<std::uint64_t>(displacement) << 16));
			emit(words, address);
		}
	} else if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U ||
		   (instruction & 0x3f20'0c00U) == 0x1d00'0800U) {
		/* FEAT_LRCPC3 LDAP1 and STL1, and LDAPUR and STLUR to SIMD&FP
		 * registers: the virtual CPU does not implement FEAT_LRCPC3. */
		return illegal_instruction(context);
	} else if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
		const unsigned opcode = (instruction >> 12) & 15U;
		const bool interleaved = opcode == 8U || opcode == 4U ||
			opcode == 0U;
		const bool valid_opcode = interleaved || opcode == 7U ||
			opcode == 10U || opcode == 6U || opcode == 2U;
		const bool post_index = (instruction & 0x0080'0000U) != 0;
		const bool valid_no_offset = post_index ||
			((instruction >> 16) & 31U) == 0U;
		const bool valid_arrangement = !interleaved ||
			(instruction & 0x4000'0c00U) != 0x0000'0c00U;
		if (valid_opcode && valid_no_offset && valid_arrangement) {
			const bool load =
				(instruction & 0x0040'0000U) != 0;
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned size = (instruction >> 10) & 3U;
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto *gadgets =
				simd_structure_gadgets(opcode, q, size);
			const auto base_offset = base_register_offset(base);
			const auto offset_register_offset =
				transfer_register_offset(offset_register);
			emit(words, (load ? gadgets->load : gadgets->store)
				[base == 31U][address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
		   (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
		const unsigned opcode = (instruction >> 13) & 7U;
		const unsigned scale = opcode >> 1;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const bool scale_bit = (instruction & 0x0000'1000U) != 0;
		const unsigned size = (instruction >> 10) & 3U;
		const bool post_index = (instruction & 0x0080'0000U) != 0;
		const bool valid_no_offset = post_index ||
			((instruction >> 16) & 31U) == 0U;
		const bool replicate = scale == 3U && load && !scale_bit;
		const bool valid_lane = scale == 0U ||
			(scale == 1U && (size & 1U) == 0U) ||
			(scale == 2U && (size & 2U) == 0U &&
			 ((size & 1U) == 0U || !scale_bit));
		if (valid_no_offset && replicate) {
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned count =
				((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
				(opcode == 6U ? 1U : 3U);
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto &gadgets = simd_replicate_gadgets(size, q);
			const auto base_offset = base_register_offset(base);
			const auto offset_register_offset =
				transfer_register_offset(offset_register);
			emit(words, gadgets[count - 1U][base == 31U]
				[address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32) |
				fault_position(context));
		} else if (valid_no_offset && scale != 3U && valid_lane) {
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned count =
				((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
				((opcode & 1U) != 0 ? 3U : 1U);
			const unsigned width_index = scale < 2U ? scale :
				size == 0U ? 2U : 3U;
			const unsigned lane = scale == 0U ?
				(static_cast<unsigned>(q) << 3) |
				(static_cast<unsigned>(scale_bit) << 2) | size :
				scale == 1U ?
				(static_cast<unsigned>(q) << 2) |
				(static_cast<unsigned>(scale_bit) << 1) | (size >> 1) :
				(static_cast<unsigned>(q) << (1U - size)) |
				static_cast<unsigned>(scale_bit);
			const unsigned lane_offset = lane << width_index;
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto &gadgets = simd_lane_gadgets(width_index);
			const auto base_offset = base_register_offset(base);
			const auto offset_register_offset =
				transfer_register_offset(offset_register);
			emit(words, (load ? gadgets[count - 1U].load :
				gadgets[count - 1U].store)[base == 31U]
				[address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32) |
				(static_cast<std::uintptr_t>(lane_offset) << 48) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xff20'0400U) == 0xf820'0400U) {
		/* LDRAA and LDRAB need FEAT_PAuth, which the virtual CPU does not
		 * implement. */
		return illegal_instruction(context);
	} else if ((instruction & 0xffc0'0000U) == 0xb940'0000U ||
		   (instruction & 0xffc0'0000U) == 0xf940'0000U ||
		   (instruction & 0xffc0'0000U) == 0xb900'0000U ||
		   (instruction & 0xffc0'0000U) == 0xf900'0000U) {
		const auto opcode = instruction & 0xffc0'0000U;
		const bool transfer_zero = (instruction & 31U) == 31U;
		const auto reg = instruction & 31U;
		const auto base = (instruction >> 5) & 31U;
		const auto scale = opcode == 0xb940'0000U ||
			opcode == 0xb900'0000U ? 2U : 3U;
		const auto operation = opcode == 0xb940'0000U ||
			opcode == 0xf940'0000U ? 1U : 0U;
		/* A register transfer takes the entry that bakes it; only XZR
		 * has a shared entry. */
		emit(words, transfer_zero ?
			scalar_unsigned_memory_gadget(
				scale, operation, base == 31U, true) :
			scalar_unsigned_memory_inplace_gadget(
				scale, operation, base == 31U, reg));
		const auto offset = ((instruction >> 10) & 0xfffU) << scale;
		const auto reg_offset = transfer_register_offset(reg);
		const auto base_offset = base_register_offset(base);
		emit(words, reg_offset |
			(static_cast<std::uintptr_t>(base_offset) << 16) |
			(static_cast<std::uintptr_t>(offset) << 32) |
			fault_position(context));
	} else if ((instruction & 0x3f20'0c00U) == 0x1900'0800U) {
		/* FEAT_LRCPC3 LDIAPP, STILP, and LDAPR and STLR with writeback,
		 * and the FEAT_LSCP LDAP, LDAPP, and STLP pairs: the virtual CPU
		 * implements neither feature. */
		return illegal_instruction(context);
	} else if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U ||
		   (instruction & 0x3f20'0c00U) == 0x1900'0000U) {
		const bool no_offset =
			(instruction & 0x3fff'fc00U) == 0x38bf'c000U;
		const unsigned size = instruction >> 30;
		const unsigned operation = no_offset ? 1U :
			(instruction >> 22) & 3U;
		const bool valid = no_offset ||
			!((operation == 2U && size == 3U) ||
			  (operation == 3U && size >= 2U));
		if (!valid)
			return illegal_instruction(context);
		const auto offset = no_offset ? std::int64_t{0} :
			sign_extend((instruction >> 12) & 0x1ffU, 9);
		const unsigned transfer = instruction & 31U;
		if (operation == 0U) {
			emit(words, rcpc_store_gadgets[size][transfer == 31U]);
		} else {
			unsigned variant = size;
			if (operation == 2U)
				variant = size == 0U ? 5U :
					size == 1U ? 7U : 8U;
			else if (operation == 3U)
				variant = size == 0U ? 4U : 6U;
			emit(words,
				rcpc_load_gadgets[variant][transfer == 31U]);
		}
		emit(words, rcpc_memory_operand(instruction, offset) |
			fault_position(context));
	} else if ((instruction & 0x3fa0'0000U) == 0x0880'0000U) {
		/* o0 clear selects LDLAR and STLLR, which need FEAT_LOR; the
		 * virtual CPU does not implement it. */
		const bool valid = ((instruction >> 16) & 31U) == 31U &&
			((instruction >> 10) & 31U) == 31U &&
			(instruction & 0x0000'8000U) != 0;
		if (valid) {
			const bool load =
				(instruction & 0x0040'0000U) != 0;
			const unsigned size = instruction >> 30;
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, load ?
				ordered_load_gadgets[size][transfer == 31U] :
				ordered_store_gadgets[size][transfer == 31U]);
			const auto transfer_offset =
				transfer_register_offset(transfer);
			const auto base_offset = base_register_offset(base);
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xbf20'0c00U) == 0x1920'0000U ||
		   (instruction & 0xbf20'0800U) == 0x1920'0800U) {
		/* FEAT_LSE128 LDCLRP, LDSETP, and SWPP, and read-check-write
		 * RCWCAS, RCWCASP, and the pair modifies: the virtual CPU
		 * implements none of FEAT_LSE128, FEAT_THE, and FEAT_D128. The
		 * scalar read-check-write modifies fall in atomic-memory cells
		 * that the atomic decoder below rejects. */
		return illegal_instruction(context);
	} else if ((instruction & 0x3f20'0c00U) == 0x1900'0400U ||
		   (instruction & 0x3f20'0c00U) == 0x1d00'0400U ||
		   (instruction & 0x3fe0'0c00U) == 0x19c0'0400U ||
		   (instruction & 0x3fe0'0c00U) == 0x1dc0'0400U) {
		/* SET and SETG occupy values also accepted by the broad CPYF/CPY
		 * masks. Resolve both specific families before selecting CPY.
		 * SETG needs FEAT_MTE, which the virtual CPU does not implement. */
		const bool set_tag =
			(instruction & 0x3fe0'0c00U) == 0x1dc0'0400U;
		const bool set = !set_tag &&
			(instruction & 0x3fe0'0c00U) == 0x19c0'0400U;
		const bool copy_forward = !set && !set_tag &&
			(instruction & 0x3f20'0c00U) == 0x1900'0400U;
		const unsigned family = set ? 2U : copy_forward ? 0U : 1U;
		const unsigned destination = instruction & 31U;
		const unsigned count = (instruction >> 5) & 31U;
		const unsigned source = (instruction >> 16) & 31U;
		const unsigned stage = set ?
			(instruction >> 14) & 3U :
			(instruction >> 22) & 3U;
		const bool valid = !set_tag && (instruction >> 30) == 0 &&
			stage != 3 && destination != 31U && count != 31U &&
			destination != count && source != destination &&
			source != count && (set || source != 31U);
		if (!valid)
			return illegal_instruction(context);
		emit(words, mops_gadget(family, stage, source == 31U));
		if (stage != 1) {
			emit(words, mops_operand(instruction));
			if (stage == 2)
				emit(words, address);
		}
	} else if ((instruction & 0xff20'0000U) == 0xd920'0000U ||
		   (instruction & 0xffc0'0000U) == 0x6880'0000U ||
		   (instruction & 0xffc0'0000U) == 0x6900'0000U ||
		   (instruction & 0xffc0'0000U) == 0x6980'0000U) {
		/* FEAT_MTE LDG/STG/ST2G/STZG/STZ2G/STGP, and the EL1 LDGM/STGM/
		 * STZGM: the virtual CPU does not implement FEAT_MTE. */
		return illegal_instruction(context);
	} else if ((instruction & 0x3f20'0c00U) == 0x3c20'0000U ||
		   (instruction & 0xffff'fc00U) == 0xf83f'd000U ||
		   (instruction & 0xffff'fc00U) == 0xf83f'9000U ||
		   (instruction & 0xffe0'fc00U) == 0xf820'b000U ||
		   (instruction & 0xffe0'fc00U) == 0xf820'a000U) {
		/* The FEAT_LSFE floating-point atomics, and FEAT_LS64 LD64B,
		 * ST64B, ST64BV, and ST64BV0: the virtual CPU implements neither
		 * feature. */
		return illegal_instruction(context);
	} else if ((instruction & 0x3f20'0c00U) == 0x1920'0400U ||
		   (instruction & 0x3f00'0000U) == 0x0900'0000U) {
		/* The FEAT_LSUI atomics, compare-and-swaps, and exclusives: the
		 * virtual CPU does not implement FEAT_LSUI. */
		return illegal_instruction(context);
	} else if ((instruction & 0x3f20'0c00U) == 0x3820'0000U) {
		const unsigned o3 = (instruction >> 15) & 1U;
		const unsigned opcode = (instruction >> 12) & 7U;
		if (!o3 || opcode == 0U) {
			const unsigned operation = o3 ? 8U : opcode;
			const unsigned size = instruction >> 30;
			const unsigned source = (instruction >> 16) & 31U;
			const unsigned destination = instruction & 31U;
			const bool acquire =
				(instruction & 0x0080'0000U) != 0 &&
				destination != 31U;
			const bool release =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(source == 31U ? 2U : 0U) |
				(destination == 31U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, atomic_rmw_gadgets[operation][ordering]
				[size][registers]);
			const auto source_offset =
				transfer_register_offset(source);
			const auto destination_offset =
				transfer_register_offset(destination);
			const auto base_offset = base_register_offset(base);
			emit(words, destination_offset |
				(static_cast<std::uintptr_t>(source_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xbfa0'0000U) == 0x0820'0000U) {
		const unsigned compare = (instruction >> 16) & 31U;
		const unsigned replacement = instruction & 31U;
		const unsigned size_code = instruction >> 30;
		const bool valid = ((instruction >> 10) & 31U) == 31U &&
			(compare & 1U) == 0 && (replacement & 1U) == 0 &&
			size_code <= 1U;
		if (valid) {
			const bool acquire =
				(instruction & 0x0040'0000U) != 0;
			const bool release =
				(instruction & 0x0000'8000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(compare == 30U ? 2U : 0U) |
				(replacement == 30U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, compare_swap_pair_gadgets[ordering]
				[size_code][registers]);
			const auto compare_offset = compare * sizeof(std::uint64_t);
			const auto replacement_offset =
				replacement * sizeof(std::uint64_t);
			const auto base_offset = base_register_offset(base);
			emit(words, compare_offset |
				(static_cast<std::uintptr_t>(replacement_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3fa0'0000U) == 0x08a0'0000U) {
		if (((instruction >> 10) & 31U) == 31U) {
			const unsigned compare = (instruction >> 16) & 31U;
			const unsigned replacement = instruction & 31U;
			const bool acquire =
				(instruction & 0x0040'0000U) != 0 &&
				compare != 31U;
			const bool release =
				(instruction & 0x0000'8000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(compare == 31U ? 2U : 0U) |
				(replacement == 31U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, compare_swap_gadgets[ordering]
				[instruction >> 30][registers]);
			const auto compare_offset =
				transfer_register_offset(compare);
			const auto replacement_offset =
				transfer_register_offset(replacement);
			const auto base_offset = base_register_offset(base);
			emit(words, compare_offset |
				(static_cast<std::uintptr_t>(replacement_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0xbfa0'0000U) == 0x8820'0000U) {
		const bool load = (instruction & 0x0040'0000U) != 0;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 10) & 31U;
		const unsigned status = (instruction >> 16) & 31U;
		const bool valid = !load ||
			(status == 31U && first != second);
		if (valid) {
			const unsigned ordering =
				(instruction & 0x0000'8000U) != 0;
			const unsigned size = (instruction >> 30) - 2U;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words, load ?
					uml_arm64_gadget_atomic_sp_alignment_read :
					uml_arm64_gadget_atomic_sp_alignment_write);
			if (load) {
				const unsigned registers =
					(first == 31U ? 2U : 0U) |
					(second == 31U ? 1U : 0U);
				emit(words, exclusive_pair_load_gadgets
					[ordering][size][registers]);
				const auto first_offset =
					transfer_register_offset(first);
				const auto second_offset =
					transfer_register_offset(second);
				const auto base_offset =
					base_register_offset(base);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32) |
					fault_position(context));
			} else {
				const unsigned registers =
					(first == 31U ? 4U : 0U) |
					(second == 31U ? 2U : 0U) |
					(status == 31U ? 1U : 0U);
				emit(words, exclusive_pair_store_gadgets
					[ordering][size][registers]);
				const auto first_offset =
					transfer_register_offset(first);
				const auto second_offset =
					transfer_register_offset(second);
				const auto status_offset =
					transfer_register_offset(status);
				const auto base_offset =
					base_register_offset(base);
				/* Four 12-bit offsets and the position: no 16-bit
				 * field is left for the base. */
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 12) |
					(static_cast<std::uintptr_t>(status_offset) << 24) |
					(static_cast<std::uintptr_t>(base_offset) << 36) |
					fault_position(context));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3fa0'0000U) == 0x0800'0000U) {
		const bool load = (instruction & 0x0040'0000U) != 0;
		const unsigned transfer = instruction & 31U;
		const unsigned status = (instruction >> 16) & 31U;
		const bool valid = ((instruction >> 10) & 31U) == 31U &&
			(!load || status == 31U);
		if (valid) {
			const unsigned ordering =
				(instruction & 0x0000'8000U) != 0;
			const unsigned size = instruction >> 30;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words, load ?
					uml_arm64_gadget_atomic_sp_alignment_read :
					uml_arm64_gadget_atomic_sp_alignment_write);
			if (load) {
				emit(words, exclusive_load_gadgets[ordering]
					[size][transfer == 31U]);
				const auto transfer_offset =
					transfer_register_offset(transfer);
				const auto base_offset =
					base_register_offset(base);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					fault_position(context));
			} else {
				const unsigned registers =
					(transfer == 31U ? 2U : 0U) |
					(status == 31U ? 1U : 0U);
				emit(words, exclusive_store_gadgets[ordering]
					[size][registers]);
				const auto transfer_offset =
					transfer_register_offset(transfer);
				const auto status_offset =
					transfer_register_offset(status);
				const auto base_offset =
					base_register_offset(base);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(status_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32) |
					fault_position(context));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		if (size_code == 3 && operation == 2) {
			/* PRFM is architecturally only a hint. */
			emit(words, uml_arm64_gadget_nop);
		} else if ((size_code >= 2 && operation == 3) ||
			   (size_code == 3 && operation > 1)) {
			return illegal_instruction(context);
		} else {
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, scalar_unsigned_memory_gadget(size_code,
				operation, base == 31U, transfer == 31U));
			const auto transfer_offset =
				transfer_register_offset(transfer);
			const auto base_offset = base_register_offset(base);
			const auto offset =
				((instruction >> 10) & 0xfffU) << size_code;
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset) << 32) |
				fault_position(context));
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		if (operation >= 2 && size_code != 0) {
			return illegal_instruction(context);
		} else {
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, vector_unsigned_memory_gadget(
				size_code, operation, base == 31U));
			const auto transfer_offset =
				vector_operand_offset(transfer);
			const auto base_offset = base_register_offset(base);
			const unsigned scale = operation >= 2U ? 4U : size_code;
			const auto offset =
				((instruction >> 10) & 0xfffU) << scale;
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset) << 32) |
				fault_position(context));
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		const bool prefetch = size_code == 3U && operation == 2U;
		const bool valid_transfer =
			(size_code != 3 || operation <= 1) &&
			(size_code != 2 || operation != 3);
		const bool register_offset =
			(instruction & 0x0020'0000U) != 0;
		const auto option = (instruction >> 13) & 7U;
		const auto mode = (instruction >> 10) & 3U;
		const bool valid_offset = !register_offset ||
			(mode == 2U && (option & 2U) != 0);
		const bool valid_prefetch = prefetch && (register_offset ?
			(mode == 2U && (option & 2U) != 0) : mode == 0U);
		if (valid_prefetch) {
			emit(words, uml_arm64_gadget_nop);
		} else if (prefetch) {
			return illegal_instruction(context);
		} else if (valid_transfer && valid_offset) {
			const auto *gadgets =
				scalar_other_gadgets(size_code, operation);
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto transfer_offset =
				transfer_register_offset(transfer);
			const auto base_offset = base_register_offset(base);
			if (register_offset) {
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const auto offset_register_offset =
					arithmetic_register_offset(offset_register);
				const unsigned extension = option == 2U ? 0U :
					option == 6U ? 2U : 1U;
				const unsigned address_mode = extension +
					((instruction & 0x1000U) != 0 ? 3U : 0U);
				emit(words, gadgets->register_offset[address_mode]
					[base == 31U][transfer == 31U]);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(
						offset_register_offset) << 32) |
					fault_position(context));
			} else {
				const bool overlap = operation != 0U && base != 31U &&
					base == transfer;
				const unsigned address_mode = mode == 1U ?
					(overlap ? 3U : 2U) : mode == 3U ?
					(overlap ? 0U : 1U) : 0U;
				emit(words, gadgets->immediate[address_mode]
					[base == 31U][transfer == 31U]);
				const auto displacement = sign_extend(
					(instruction >> 12) & 0x1ffU, 9);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					((static_cast<std::uint64_t>(displacement) &
					  0xffffU) << 32) |
					fault_position(context));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		const bool valid_transfer = operation < 2 || size_code == 0;
		const bool register_offset =
			(instruction & 0x0020'0000U) != 0;
		const auto option = (instruction >> 13) & 7U;
		const auto mode = (instruction >> 10) & 3U;
		const bool valid_offset = register_offset ?
			(mode == 2U && (option & 2U) != 0) : mode != 2U;
		if (valid_transfer && valid_offset) {
			const auto *gadgets =
				vector_other_gadgets(size_code, operation);
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto transfer_offset =
				vector_operand_offset(transfer);
			const auto base_offset = base_register_offset(base);
			if (register_offset) {
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const auto offset_register_offset =
					arithmetic_register_offset(offset_register);
				const unsigned extension = option == 2U ? 0U :
					option == 6U ? 2U : 1U;
				const unsigned address_mode = extension +
					((instruction & 0x1000U) != 0 ? 3U : 0U);
				emit(words, gadgets->register_offset[address_mode]
					[base == 31U]);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(
						offset_register_offset) << 32) |
					fault_position(context));
			} else {
				const unsigned address_mode = mode == 1U ? 2U :
					mode == 3U ? 1U : 0U;
				emit(words, gadgets->immediate[address_mode]
					[base == 31U]);
				const auto displacement = sign_extend(
					(instruction >> 12) & 0x1ffU, 9);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					((static_cast<std::uint64_t>(displacement) &
					  0xffffU) << 32) |
					fault_position(context));
			}
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto operation = instruction >> 30;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const auto mode = (instruction >> 23) & 3U;
		const auto first = instruction & 31U;
		const auto second = (instruction >> 10) & 31U;
		/* opc 11 holds the FEAT_LSUI LDTP, STTP, LDTNP, and STTNP, which
		 * the virtual CPU does not implement. */
		const bool valid = operation != 3U &&
			(operation != 1U || (load && mode != 0U)) &&
			(!load || first != second);
		if (valid) {
			const unsigned base = (instruction >> 5) & 31U;
			const bool overlap = load && base != 31U &&
				(base == first || base == second);
			const unsigned address_mode = mode == 1U ?
				(overlap ? 3U : 2U) : mode == 3U ?
				(overlap ? 0U : 1U) : 0U;
			const unsigned register_mode =
				(first == 31U ? 2U : 0U) +
				(second == 31U ? 1U : 0U);
			const auto *gadgets =
				integer_pair_gadgets(operation, load);
			const auto first_offset =
				transfer_register_offset(first);
			const auto second_offset =
				transfer_register_offset(second);
			const auto base_offset = base_register_offset(base);
			const auto displacement = sign_extend(
				(instruction >> 15) & 0x7fU, 7) <<
				(operation >= 2U ? 3U : 2U);
			emit(words, (*gadgets)[address_mode][base == 31U]
				[register_mode]);
			emit(words, first_offset |
				(static_cast<std::uintptr_t>(second_offset) << 9) |
				((static_cast<std::uint64_t>(displacement) & 0x3fffU)
					<< 18) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
		   (instruction & 0x0400'0000U) != 0) {
		const auto operation = instruction >> 30;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const auto first = instruction & 31U;
		const auto second = (instruction >> 10) & 31U;
		/* opc 11 holds the FEAT_LSUI LDTP, STTP, LDTNP, and STTNP. */
		if (operation != 3U && (!load || first != second)) {
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned mode = (instruction >> 23) & 3U;
			const unsigned address_mode = mode == 1U ? 2U :
				mode == 3U ? 1U : 0U;
			const auto &gadgets =
				vector_pair_gadgets(operation, load);
			const auto first_offset = vector_operand_offset(first);
			const auto second_offset = vector_operand_offset(second);
			const auto base_offset = base_register_offset(base);
			const auto displacement = sign_extend(
				(instruction >> 15) & 0x7fU, 7) <<
				(operation < 2U ? operation + 2U : 4U);
			emit(words, gadgets[address_mode][base == 31U]);
			/* Three 12-bit offsets and a signed 12-bit displacement
			 * fill the low 48 bits; the position takes the top. */
			emit(words, first_offset |
				(static_cast<std::uintptr_t>(second_offset) << 12) |
				(static_cast<std::uintptr_t>(base_offset) << 24) |
				((static_cast<std::uint64_t>(displacement) & 0xfffU)
					<< 36) |
				fault_position(context));
		} else {
			return illegal_instruction(context);
		}
	} else {
		return implementation_gap(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
