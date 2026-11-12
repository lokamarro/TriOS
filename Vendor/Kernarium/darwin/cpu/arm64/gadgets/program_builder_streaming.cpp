// SPDX-License-Identifier: GPL-2.0-or-later
/* Selection of the generated streaming gadgets
 * (engine/generate_streaming.py) for a program translated for SVCR.SM = 1:
 * the SVE, SVE2 and SME forms the encoding table's rows name, and the FP&SIMD
 * classes that execute in streaming mode. The generated header gives each
 * form its entries, the instruction bits that select among them and the
 * operands whose offsets the stream carries; a lowered form's immediate, or
 * a count derived from the run-time SVL, follows them. A load or store takes
 * a vector memory gadget instead, with its address mode, registers and
 * shape resolved into the stream sme_layout.h describes. */
#include "program_builder_decode.hpp"
#include "gadget_selection_api.hpp"
#include "sme_layout.h"
#include "streaming_forms.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>

extern "C" {
/* engine/sme.inc: the generated entries, in the order of the header. */
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries_end[];
extern const std::uintptr_t uml_arm64_gadget_sm_memory_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_memory_entries_end[];
}

namespace uml::cpu::arm64::gadgets::builder_detail {
namespace {

using detail::SmeOperand;
using detail::SmeOperandKind;
using selection::emit;
using streaming::Form;
using streaming::Lowering;

/* The word's bits under `mask`, packed from the least significant up. */
[[nodiscard]] constexpr std::uint32_t gather(std::uint32_t word,
	std::uint32_t mask) noexcept
{
	std::uint32_t result = 0;
	for (unsigned position = 0; mask != 0U; mask &= mask - 1U, ++position)
		result |= ((word >> std::countr_zero(mask)) & 1U) << position;
	return result;
}

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint32_t value,
	unsigned width) noexcept
{
	const auto shift = 64U - width;
	return static_cast<std::int64_t>(std::uint64_t{value} << shift) >> shift;
}

/* DecodePredCount: the elements of `esize` bits a predicate pattern names
 * at this SVL. */
[[nodiscard]] constexpr std::uint64_t pattern_count(unsigned pattern,
	unsigned esize, unsigned svl) noexcept
{
	return predicate_pattern_count(pattern, svl * 8U / esize);
}

/* The element size, in bits, that an immediate shift's tsz field selects,
 * and the field joined to imm3, for the three field layouts. */
struct ShiftImmediate final {
	unsigned esize;
	unsigned value;
};

[[nodiscard]] constexpr ShiftImmediate shift_immediate(std::uint32_t word,
	unsigned layout) noexcept
{
	unsigned tsz;
	unsigned imm3;
	if (layout == 1) {	/* predicated: tszh 23:22, tszl 9:8, imm3 7:5 */
		tsz = ((word >> 20) & 0xcU) | ((word >> 8) & 3U);
		imm3 = (word >> 5) & 7U;
	} else if (layout == 2) {	/* narrow: tszh 22, tszl 20:19 */
		tsz = ((word >> 20) & 4U) | ((word >> 19) & 3U);
		imm3 = (word >> 16) & 7U;
	} else {	/* tszh 23:22, tszl 20:19, imm3 18:16 */
		tsz = ((word >> 20) & 0xcU) | ((word >> 19) & 3U);
		imm3 = (word >> 16) & 7U;
	}
	const unsigned esize = 8U << (std::bit_width(tsz) - 1U);
	return {esize, (tsz << 3) | imm3};
}

struct Lowered final {
	std::array<std::uint64_t, 3> immediates{};
	/* The variant an immediate shift's element size selects. */
	std::uint32_t variant = 0;
};

/* A lowered form's immediates, or nothing for a word whose immediate is
 * reserved. */
[[nodiscard]] std::optional<Lowered> lower(const Form &form,
	std::uint32_t word, unsigned svl) noexcept
{
	Lowered result;
	auto &immediate = result.immediates;
	const unsigned size = (word >> 22) & 3U;
	const unsigned esize = 8U << size;
	switch (form.lowering) {
	case Lowering::none:
	case Lowering::plain:
		break;
	case Lowering::logical_immediate: {
		const auto bits = detail::decode_logical_immediate(true,
			(word >> 17) & 1U, (word >> 11) & 63U, (word >> 5) & 63U);
		if (!bits)
			return std::nullopt;
		immediate[0] = *bits;
		break;
	}
	case Lowering::index_immediates:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 31U, 5));
		immediate[1] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::index_start:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 31U, 5));
		break;
	case Lowering::index_step:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::arith_unsigned:
		immediate[0] = std::uint64_t{(word >> 5) & 0xffU} <<
			(((word >> 13) & 1U) != 0U ? 8U : 0U);
		break;
	case Lowering::arith_signed:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0xffU, 8));
		break;
	case Lowering::arith_byte:
		immediate[0] = (word >> 5) & 0xffU;
		break;
	case Lowering::arith_saturating: {
		/* The unsigned immediate as three parts that each fit a
		 * positive element of this size. */
		auto remaining = std::uint64_t{(word >> 5) & 0xffU} <<
			(((word >> 13) & 1U) != 0U ? 8U : 0U);
		const auto largest = (std::uint64_t{1} << (esize - 1U)) - 1U;
		for (auto &part : immediate) {
			part = std::min(remaining, largest);
			remaining -= part;
		}
		if (remaining != 0U)
			return std::nullopt;
		break;
	}
	case Lowering::dup_immediate:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0xffU, 8) *
			(((word >> 13) & 1U) != 0U ? 256 : 1));
		break;
	case Lowering::fp_immediate:
		immediate[0] = selection::expand_vfp_immediate(
			static_cast<std::uint8_t>((word >> 5) & 0xffU), esize);
		break;
	case Lowering::compare_signed:
		immediate[0] = static_cast<std::uint64_t>(
			sign_extend((word >> 16) & 31U, 5));
		break;
	case Lowering::compare_unsigned:
		immediate[0] = (word >> 14) & 0x7fU;
		break;
	case Lowering::element_count:
		immediate[0] = pattern_count((word >> 5) & 31U, esize, svl) *
			(((word >> 16) & 15U) + 1U);
		break;
	case Lowering::predicate_count:
		immediate[0] = pattern_count((word >> 5) & 31U, esize, svl);
		break;
	case Lowering::extract: {
		/* EXT's byte offset; one past the vector is offset 0. */
		const unsigned offset = (((word >> 16) & 31U) << 3) |
			((word >> 10) & 7U);
		immediate[0] = offset < svl ? offset : 0U;
		break;
	}
	case Lowering::broadcast_index: {
		const unsigned field = (((word >> 22) & 3U) << 5) |
			((word >> 16) & 31U);
		const unsigned lowest =
			static_cast<unsigned>(std::countr_zero(field & 31U));
		const unsigned index = field >> (lowest + 1U);
		/* A quadword selects its doubleword pair. */
		immediate[0] = lowest == 4U ? 2U * index : index;
		break;
	}
	case Lowering::shift_right:
	case Lowering::shift_right_negated:
	case Lowering::shift_left:
	case Lowering::rotate_right: {
		const auto shift = shift_immediate(word, form.argument);
		const bool left = form.lowering == Lowering::shift_left;
		const auto amount = left ? shift.value - shift.esize :
			2U * shift.esize - shift.value;
		immediate[0] = form.lowering == Lowering::shift_right_negated ?
			static_cast<std::uint64_t>(-std::int64_t{amount}) : amount;
		if (form.lowering == Lowering::rotate_right)
			immediate[1] = shift.esize - amount;
		result.variant = static_cast<std::uint32_t>(
			std::countr_zero(shift.esize)) - 3U;
		break;
	}
	case Lowering::fp_conditional_compare:
		immediate[0] = std::uint64_t{word & 15U} << 28;
		break;
	case Lowering::fp_fixed_to_float: {
		/* 2^-fbits in the destination's precision. */
		const unsigned fbits = 64U - ((word >> 10) & 63U);
		immediate[0] = form.argument != 0U ?
			std::uint64_t{1023U - fbits} << 52 :
			std::uint64_t{127U - fbits} << 23;
		break;
	}
	}
	return result;
}

[[nodiscard]] unsigned immediate_count(Lowering lowering) noexcept
{
	switch (lowering) {
	case Lowering::none:
	case Lowering::plain:
		return 0;
	case Lowering::index_immediates:
	case Lowering::rotate_right:
		return 2;
	case Lowering::arith_saturating:
		return 3;
	default:
		return 1;
	}
}

/* The offset a slot's register has: in the register file for Z, P and V,
 * in CpuState for a general register, where XZR reads the zero slot. */
[[nodiscard]] std::uint64_t slot_offset(SmeOperandKind kind, unsigned number,
	unsigned svl) noexcept
{
	switch (kind) {
	case SmeOperandKind::z:
	case SmeOperandKind::v:
		return sme_z_offset(svl, number);
	case SmeOperandKind::p:
		return sme_p_offset(svl, number);
	case SmeOperandKind::gpr:
		return number == 31U ? UML_GADGET_CPU_ZERO :
			std::uint64_t{number} * sizeof(std::uint64_t);
	case SmeOperandKind::gpr_or_sp:
	case SmeOperandKind::za_tile:
		break;
	}
	return std::uint64_t{number} * sizeof(std::uint64_t);
}

/* The 16-bit offsets of every register the form's operands name, in
 * operand order with a group's registers in turn; ZA tiles are part of
 * the selected entry. */
[[nodiscard]] std::uint64_t slot_offsets(const SmeOperand *operands,
	unsigned count, std::uint32_t word, unsigned svl, bool &any) noexcept
{
	std::uint64_t packed = 0;
	unsigned slot = 0;
	for (unsigned index = 0; index < count; ++index) {
		const auto &operand = operands[index];
		if (operand.kind == SmeOperandKind::za_tile)
			continue;
		const auto number = detail::sme_operand_number(operand, word);
		const unsigned registers =
			operand.kind == SmeOperandKind::p ? 16U : 32U;
		for (unsigned member = 0; member < operand.count; ++member) {
			const auto offset = slot_offset(operand.kind,
				(number + member * operand.stride) % registers, svl);
			packed |= offset << (16U * slot++);
		}
	}
	any = slot != 0U;
	return packed;
}

[[nodiscard]] std::span<const std::uintptr_t> generated_entries() noexcept
{
	return {uml_arm64_gadget_sm_generated_entries,
		uml_arm64_gadget_sm_generated_entries_end};
}

/* Emits one generated form for the context's word, or reports a word the
 * form cannot take. */
[[nodiscard]] bool emit_form(DecodeContext &context, const Form &form,
	const SmeOperand *operands)
{
	const auto word = context.instruction;
	const unsigned svl = context.options.host_capabilities.sme_svl_bytes;
	const auto lowered = lower(form, word, svl);
	if (!lowered)
		return false;
	const auto variant = gather(word, form.variant_mask) + lowered->variant;
	if (variant >= form.variants)
		return false;
	const auto entries = generated_entries();
	if (form.first_entry + variant >= entries.size())
		return false;
	bool any = false;
	const auto offsets = slot_offsets(operands, form.operand_count, word,
		svl, any);
	emit(context.words, entries[form.first_entry + variant]);
	if (any)
		emit(context.words, static_cast<std::uintptr_t>(offsets));
	for (unsigned index = 0; index < immediate_count(form.lowering); ++index)
		emit(context.words,
			static_cast<std::uintptr_t>(lowered->immediates[index]));
	return true;
}

/* The FP&SIMD class a word belongs to, as classify_fp_simd_streaming()
 * finds it, or the class count for none. */
[[nodiscard]] std::size_t fp_simd_class(std::uint32_t word) noexcept
{
	const std::uint32_t bucket = word >> detail::sme_encoding_bucket_shift;
	for (std::uint32_t slot = detail::fp_simd_streaming_buckets[bucket];
	     slot < detail::fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
		const auto index = detail::fp_simd_streaming_bucket_rows[slot];
		const auto &row = detail::fp_simd_streaming_classes[index];
		if ((word & row.mask) == row.value)
			return index;
	}
	return detail::fp_simd_streaming_classes.size();
}

/* The words of a vector memory gadget's stream after its entry, less the
 * PC (sme_layout.h). */
struct VectorStream final {
	std::uint64_t address = 0;
	std::uint64_t immediate = 0;
	std::uint64_t transfer = 0;
	std::uint64_t shape = 0;
};

[[nodiscard]] constexpr std::uint64_t general_offset(unsigned number) noexcept
{
	return number == 31U ? UML_GADGET_CPU_ZERO :
		std::uint64_t{number} * sizeof(std::uint64_t);
}

/* Word 0 for a base register (31 is SP) spanning `footprint` bytes. */
[[nodiscard]] constexpr std::uint64_t base_word(unsigned base,
	std::uint64_t footprint) noexcept
{
	return std::uint64_t{base} * sizeof(std::uint64_t) |
		footprint << UML_GADGET_SME_VM_FOOTPRINT_SHIFT |
		std::uint64_t{base == 31U} << UML_GADGET_SME_VM_BASE_SP_BIT;
}

/* Word 0's index register, shifted left by `shift`. */
[[nodiscard]] constexpr std::uint64_t index_bits(unsigned index,
	unsigned shift) noexcept
{
	return general_offset(index) << 16 |
		std::uint64_t{shift} << UML_GADGET_SME_VM_SHIFT_SHIFT |
		std::uint64_t{1} << UML_GADGET_SME_VM_INDEX_BIT;
}

[[nodiscard]] constexpr std::uint64_t shape_word(unsigned shape, bool store,
	unsigned msize, unsigned esize, bool sign, unsigned registers) noexcept
{
	return std::uint64_t{shape} << UML_GADGET_SME_VM_SHAPE_SHIFT |
		std::uint64_t{store} << UML_GADGET_SME_VM_STORE_BIT |
		std::uint64_t(std::countr_zero(msize)) <<
			UML_GADGET_SME_VM_MSIZE_SHIFT |
		std::uint64_t(std::countr_zero(esize)) <<
			UML_GADGET_SME_VM_ESIZE_SHIFT |
		std::uint64_t{sign} << UML_GADGET_SME_VM_SIGN_BIT |
		std::uint64_t{registers} << UML_GADGET_SME_VM_REGISTERS_SHIFT;
}

[[nodiscard]] bool emit_vector_memory(DecodeContext &context,
	std::size_t entry, const VectorStream &stream)
{
	const std::span<const std::uintptr_t> entries{
		uml_arm64_gadget_sm_memory_entries,
		uml_arm64_gadget_sm_memory_entries_end};
	if (entry >= entries.size())
		return false;
	emit(context.words, entries[entry]);
	for (const auto word : {stream.address, stream.immediate,
		     stream.transfer, stream.shape})
		emit(context.words, static_cast<std::uintptr_t>(word));
	emit(context.words, context.address);
	return true;
}

/* The signed or unsigned immediate of a memory form. */
[[nodiscard]] std::int64_t memory_immediate(const detail::SmeMemory &memory,
	std::uint32_t word) noexcept
{
	std::uint32_t value = 0;
	unsigned width = 0;
	for (unsigned index = 0; index < memory.immediate_segment_count;
	     ++index) {
		const auto &segment = memory.immediate[index];
		value = (value << segment.width) |
			detail::sme_segment_value(segment, word);
		width += segment.width;
	}
	if (width == 0U)
		return 0;
	return memory.immediate_signed ? sign_extend(value, width) :
		static_cast<std::int64_t>(value);
}

[[nodiscard]] constexpr unsigned memory_shape(
	detail::SmeMemoryShape shape) noexcept
{
	switch (shape) {
	case detail::SmeMemoryShape::structure:
		return UML_GADGET_SME_VM_STRUCTURE;
	case detail::SmeMemoryShape::replicate:
		return UML_GADGET_SME_VM_REPLICATE;
	case detail::SmeMemoryShape::replicate_quad:
		return UML_GADGET_SME_VM_REPLICATE_QUAD;
	case detail::SmeMemoryShape::fill_z:
		return UML_GADGET_SME_VM_FILL_Z;
	case detail::SmeMemoryShape::fill_p:
		return UML_GADGET_SME_VM_FILL_P;
	case detail::SmeMemoryShape::za_slice:
		return UML_GADGET_SME_VM_ZA_SLICE;
	default:
		return UML_GADGET_SME_VM_CONTIGUOUS;
	}
}

/* The stream of an SVE or SME load or store at this SVL. */
[[nodiscard]] std::optional<VectorStream> vector_stream(
	const detail::SmeEncoding &row, const detail::SmeMemory &memory,
	std::uint32_t word, unsigned svl, unsigned &variant) noexcept
{
	const auto *const operands = detail::sme_encoding_operands_of(row);
	const unsigned esize = detail::sme_memory_esize(memory, word) / 8U;
	const unsigned msize = memory.msize / 8U;
	const unsigned registers = memory.registers;
	const auto shape = memory_shape(memory.shape);
	std::uint64_t footprint;
	switch (shape) {
	case UML_GADGET_SME_VM_STRUCTURE:
	case UML_GADGET_SME_VM_FILL_Z:
	case UML_GADGET_SME_VM_ZA_SLICE:
		footprint = std::uint64_t{registers} * svl;
		break;
	case UML_GADGET_SME_VM_REPLICATE:
		footprint = msize;
		break;
	case UML_GADGET_SME_VM_REPLICATE_QUAD:
		footprint = 16U;
		break;
	case UML_GADGET_SME_VM_FILL_P:
		footprint = svl / 8U;
		break;
	default:
		footprint = std::uint64_t{svl} / esize * msize;
		break;
	}
	VectorStream stream;
	const auto base = detail::sme_operand_number(
		operands[memory.base_operand], word);
	stream.address = base_word(base, footprint);
	const auto immediate = memory_immediate(memory, word);
	switch (memory.offset) {
	case detail::SmeAddressOffset::immediate_footprint:
		stream.immediate = static_cast<std::uint64_t>(immediate) *
			footprint;
		break;
	case detail::SmeAddressOffset::immediate_element:
		stream.immediate = static_cast<std::uint64_t>(immediate) * msize;
		break;
	case detail::SmeAddressOffset::immediate_16:
		stream.immediate = static_cast<std::uint64_t>(immediate) * 16U;
		break;
	case detail::SmeAddressOffset::register_element:
		stream.address |= index_bits(detail::sme_operand_number(
			operands[memory.index_operand], word),
			static_cast<unsigned>(std::countr_zero(msize)));
		break;
	case detail::SmeAddressOffset::none:
	case detail::SmeAddressOffset::vector_index:
	case detail::SmeAddressOffset::register_byte:
		break;
	}
	std::uint64_t predicate = 0;
	unsigned slot = 0;
	unsigned tile = 0;
	unsigned slice = 12;
	for (unsigned index = 0; index < row.operand_count; ++index) {
		if (index == memory.base_operand || index == memory.index_operand)
			continue;
		const auto &operand = operands[index];
		const auto number = detail::sme_operand_number(operand, word);
		switch (operand.kind) {
		case SmeOperandKind::z:
			for (unsigned k = 0; k < operand.count; ++k)
				stream.transfer |= std::uint64_t{sme_z_offset(
					svl, (number + k * operand.stride) % 32U)}
					<< (16U * slot++);
			break;
		case SmeOperandKind::p:
			if (shape == UML_GADGET_SME_VM_FILL_P)
				stream.transfer |= std::uint64_t{sme_p_offset(
					svl, number)} << (16U * slot++);
			else
				predicate = sme_p_offset(svl, number);
			break;
		case SmeOperandKind::gpr:
			slice = number;
			break;
		case SmeOperandKind::za_tile:
			tile = number;
			break;
		default:
			return std::nullopt;
		}
	}
	if (shape == UML_GADGET_SME_VM_ZA_SLICE) {
		/* The slice offset fills the bits below the tile number:
		 * four for bytes down to none for quadwords. */
		const unsigned offset_bits = 4U -
			static_cast<unsigned>(std::countr_zero(esize));
		const bool vertical = ((word >> 15) & 1U) != 0U;
		stream.transfer = general_offset(slice) |
			std::uint64_t{word & ((1U << offset_bits) - 1U)} << 16 |
			std::uint64_t{tile} << 32 |
			std::uint64_t{vertical} << 48;
		variant = tile * 2U + (vertical ? 1U : 0U);
	}
	stream.shape = predicate | shape_word(shape,
		memory.direction == detail::SmeMemoryDirection::store, msize,
		esize, memory.sign_extend, registers);
	return stream;
}

/* The stream of a SIMD&FP load or store streaming mode keeps (LDR, STR,
 * LDUR, STUR, LDP, STP, LDNP, STNP and LDR literal), and its entry, or
 * nothing for any other word. The base decoder has already rejected the
 * reserved forms of these classes. */
[[nodiscard]] std::optional<VectorStream> fp_stream(std::uint32_t word,
	std::uint64_t pc, unsigned svl, std::size_t &entry) noexcept
{
	const unsigned rt = word & 31U;
	const unsigned rn = (word >> 5) & 31U;
	VectorStream stream;
	unsigned bytes;
	unsigned registers = 1;
	bool store;
	if ((word & 0x3f00'0000U) == 0x1c00'0000U) {
		/* LDR (literal): an absolute address on the zero slot. */
		const unsigned opc = word >> 30;
		if (opc == 3U)
			return std::nullopt;
		bytes = 4U << opc;
		store = false;
		stream.address = UML_GADGET_CPU_ZERO |
			std::uint64_t{bytes} << UML_GADGET_SME_VM_FOOTPRINT_SHIFT;
		stream.immediate = pc + static_cast<std::uint64_t>(
			sign_extend((word >> 5) & 0x7ffffU, 19) * 4);
	} else if ((word & 0x3e00'0000U) == 0x2c00'0000U) {
		/* LDP, STP, LDNP and STNP: 23-24 select the mode. */
		const unsigned opc = word >> 30;
		if (opc == 3U)
			return std::nullopt;
		bytes = 4U << opc;
		registers = 2;
		store = ((word >> 22) & 1U) == 0U;
		const unsigned mode = (word >> 23) & 3U;
		stream.address = base_word(rn, 2U * bytes);
		stream.immediate = static_cast<std::uint64_t>(
			sign_extend((word >> 15) & 0x7fU, 7) * bytes);
		if (mode == 1U || mode == 3U)
			stream.address |= std::uint64_t{1} <<
				UML_GADGET_SME_VM_WRITEBACK_BIT;
		if (mode == 1U)
			stream.address |= std::uint64_t{1} <<
				UML_GADGET_SME_VM_POST_BIT;
		stream.transfer = std::uint64_t{sme_z_offset(svl,
			(word >> 10) & 31U)} << 16;
	} else if ((word & 0x3e00'0000U) == 0x3c00'0000U) {
		const unsigned size = word >> 30;
		const unsigned opc = (word >> 22) & 3U;
		if ((opc & 2U) != 0U && size != 0U)
			return std::nullopt;
		bytes = (opc & 2U) != 0U ? 16U : 1U << size;
		store = (opc & 1U) == 0U;
		stream.address = base_word(rn, bytes);
		if ((word & 0x0100'0000U) != 0U) {
			stream.immediate = std::uint64_t{(word >> 10) & 0xfffU} *
				bytes;
		} else if ((word & 0x0020'0000U) != 0U) {
			/* Register offset: option 010 UXTW, 011 LSL, 110 SXTW
			 * and 111 SXTX, shifted by the size when S is set. */
			const unsigned option = (word >> 13) & 7U;
			if (((word >> 10) & 3U) != 2U || (option & 2U) == 0U)
				return std::nullopt;
			const auto scaled = ((word >> 12) & 1U) != 0U;
			const auto shift = scaled ? static_cast<unsigned>(
				std::countr_zero(bytes)) : 0U;
			stream.address |= index_bits((word >> 16) & 31U, shift);
			if ((option & 1U) == 0U)
				stream.address |= std::uint64_t{1} <<
					UML_GADGET_SME_VM_WORD_BIT;
			if ((option & 4U) != 0U)
				stream.address |= std::uint64_t{1} <<
					UML_GADGET_SME_VM_SIGNED_BIT;
		} else {
			/* 00 unscaled, 01 post-indexed, 11 pre-indexed. */
			const unsigned mode = (word >> 10) & 3U;
			if (mode == 2U)
				return std::nullopt;
			stream.immediate = static_cast<std::uint64_t>(
				sign_extend((word >> 12) & 0x1ffU, 9));
			if (mode != 0U)
				stream.address |= std::uint64_t{1} <<
					UML_GADGET_SME_VM_WRITEBACK_BIT;
			if (mode == 1U)
				stream.address |= std::uint64_t{1} <<
					UML_GADGET_SME_VM_POST_BIT;
		}
	} else {
		return std::nullopt;
	}
	stream.transfer |= sme_z_offset(svl, rt);
	stream.shape = shape_word(UML_GADGET_SME_VM_FP, store, bytes, bytes,
		false, registers);
	const auto size = static_cast<unsigned>(std::countr_zero(bytes));
	entry = registers == 2U ?
		streaming::fp_memory_pair[(size - 2U) * 2U + store] :
		streaming::fp_memory_single[size * 2U + store];
	return stream;
}

static_assert(streaming::sve_row_forms.size() ==
	detail::sme_encodings.size());
static_assert(streaming::memory_row_forms.size() ==
	detail::sme_encodings.size());
static_assert(streaming::fp_class_forms.size() ==
	detail::fp_simd_streaming_classes.size());

} // namespace

std::optional<DecodeResult> decode_streaming_sve(DecodeContext &context,
	const detail::SmeEncoding &row)
{
	const auto index = static_cast<std::size_t>(
		&row - detail::sme_encodings.data());
	const auto form_index = streaming::sve_row_forms[index];
	if (form_index == streaming::no_form)
		return std::nullopt;
	const auto &form = streaming::forms[form_index];
	if (!emit_form(context, form, detail::sme_encoding_operands_of(row)))
		return illegal_instruction(context);
	return DecodeResult::decoded;
}

std::optional<DecodeResult> decode_streaming_memory(DecodeContext &context,
	const detail::SmeEncoding &row)
{
	const auto *const memory = detail::sme_encoding_memory_of(row);
	if (memory == nullptr)
		return std::nullopt;
	if (memory->shape == detail::SmeMemoryShape::prefetch) {
		/* PRFB, PRFH, PRFW and PRFD: a hint, which never faults. */
		emit(context.words, selection::uml_arm64_gadget_nop);
		return DecodeResult::decoded;
	}
	const auto index = static_cast<std::size_t>(
		&row - detail::sme_encodings.data());
	const auto form_index = streaming::memory_row_forms[index];
	if (form_index == streaming::no_form)
		return std::nullopt;
	const auto &form = streaming::memory_forms[form_index];
	const auto word = context.instruction;
	unsigned variant = 0;
	const auto stream = vector_stream(row, *memory, word,
		context.options.host_capabilities.sme_svl_bytes, variant);
	if (form.variant == streaming::MemoryVariant::element_size)
		variant = detail::sme_segment_value(memory->esize_shift, word) -
			form.base;
	if (!stream || variant >= form.variants ||
	    !emit_vector_memory(context, form.first_entry + variant, *stream))
		return illegal_instruction(context);
	return DecodeResult::decoded;
}

DecodeResult decode_streaming_fallback(DecodeContext &context)
{
	const auto index = fp_simd_class(context.instruction);
	if (index < streaming::fp_class_forms.size() &&
	    !detail::fp_simd_streaming_classes[index].advsimd) {
		const auto form_index = streaming::fp_class_forms[index];
		if (form_index != streaming::no_form) {
			const auto &form = streaming::forms[form_index];
			if (emit_form(context, form,
				    streaming::fp_operands.data() +
					    form.operand_begin))
				return DecodeResult::decoded;
			return illegal_instruction(context);
		}
		std::size_t entry = 0;
		if (const auto stream = fp_stream(context.instruction,
			    context.address,
			    context.options.host_capabilities.sme_svl_bytes,
			    entry))
			if (emit_vector_memory(context, entry, *stream))
				return DecodeResult::decoded;
	}
	return illegal_instruction(context);
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
