// SPDX-License-Identifier: GPL-2.0-or-later
/* Translation of the SME instructions the gadget engine implements, and the
 * check that keeps every program translated for SVCR.SM = 1 within entries
 * that are safe in streaming mode. decode_branch_system reaches the decoders
 * here only from its cold paths, where a virtual CPU without SME ends the
 * block exactly as it always did. */
#include "program_builder_decode.hpp"
#include "gadget_operand_encoding.hpp"
#include "sme_layout.h"
#include "software_helpers.hpp"

#include "cpu/arm64/detail/sm0_vector_length.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

extern "C" {
void uml_arm64_gadget_sme_svcr_read();
void uml_arm64_gadget_sme_tpidr2_read();
void uml_arm64_gadget_sme_tpidr2_write_r();
void uml_arm64_gadget_sme_tpidr2_write_z();
void uml_arm64_gadget_sme_rdsvl();
void uml_arm64_gadget_sme_addsvl();
void uml_arm64_gadget_sme_addspl();
void uml_arm64_gadget_sme_svcr_write_immediate();
void uml_arm64_gadget_sme_svcr_write_register();
void uml_arm64_gadget_sm_za_vector_load();
void uml_arm64_gadget_sm_za_vector_store();
void uml_arm64_gadget_sm_zt0_load();
void uml_arm64_gadget_sm_zt0_store();
void uml_arm64_gadget_sm_zt0_zero();
void uml_arm64_gadget_sm_za_zero_table();
void uml_arm64_gadget_nsm_za_memory();

/* engine/sme.inc: the entries a streaming program may select. */
struct UmlArm64GadgetStreamingRow {
	std::uintptr_t begin;
	std::uintptr_t end;
};
extern const UmlArm64GadgetStreamingRow uml_arm64_gadget_streaming_rows_begin[];
extern const UmlArm64GadgetStreamingRow uml_arm64_gadget_streaming_rows_end[];
extern const uml::cpu::arm64::gadgets::builder_detail::StreamingTwin
	uml_arm64_sme_twin_begin[];
extern const uml::cpu::arm64::gadgets::builder_detail::StreamingTwin
	uml_arm64_sme_twin_end[];
}

namespace uml::cpu::arm64::gadgets::builder_detail {
namespace {

using selection::emit;

/* The equal-stride table of ZERO { mask } entries in engine/sme.inc. */
constexpr std::uintptr_t za_zero_stride = 16;

[[nodiscard]] bool sme_implemented(const DecodeContext &context) noexcept
{
	return implements_sme(context.options.host_capabilities);
}

[[nodiscard]] std::uintptr_t register_offset(unsigned number) noexcept
{
	/* Number 31 is SP where the operand allows it: CpuState::sp follows
	 * the 31 general registers. */
	return std::uintptr_t{number} * sizeof(std::uint64_t);
}

[[nodiscard]] std::uintptr_t signed_immediate(std::uint32_t field,
	unsigned width) noexcept
{
	const auto shift = 64U - width;
	return static_cast<std::uintptr_t>(
		static_cast<std::int64_t>(std::uint64_t{field} << shift) >> shift);
}

/* An SVCR write ends its block: the successor is translated for the mode
 * it leaves. */
DecodeResult emit_svcr_write(DecodeContext &context, std::uint8_t mask,
	std::uint8_t value)
{
	emit(context.words, selection::Gadget{
		uml_arm64_gadget_sme_svcr_write_immediate});
	emit(context.words, std::uintptr_t{mask} |
		(std::uintptr_t{value} << UML_GADGET_SME_SVCR_VALUE_SHIFT));
	emit(context.words, context.address);
	++context.compiled;
	return DecodeResult::terminal;
}

/* A spill word of the SM = 0 vector-length family
 * (detail/sm0_vector_length.hpp), as the memory access it is. A vector
 * register Z is the V register, so LDR and STR of Z are the 16-byte SIMD&FP
 * load and store with the byte offset in the operand's signed 16-bit
 * displacement, which holds the nine-bit immediate times the register's
 * size, and the block position in its top byte as for every memory form.
 * The virtual CPU has no predicate state at SM = 0, so STR of P is a halfword
 * store of the zero register and LDR of P a halfword load into it, which
 * performs the access and discards the data. */
DecodeResult emit_sm0_spill(DecodeContext &context,
	const detail::Sm0Spill &spill)
{
	auto &words = context.words;
	const bool sp_base = spill.base == 31U;
	const auto displacement = (static_cast<std::uint64_t>(
		static_cast<std::int64_t>(spill.offset)) & 0xffffU) << 32;
	const auto base = selection::base_register_offset(spill.base) << 16;
	if (spill.predicate) {
		const auto *gadgets = selection::scalar_other_gadgets(1U,
			spill.store ? 0U : 1U);
		emit(words, gadgets->immediate[0][sp_base ? 1 : 0][1]);
		emit(words, selection::transfer_register_offset(31U) | base |
			displacement | fault_position(context));
	} else {
		const auto *gadgets = selection::vector_other_gadgets(0U,
			spill.store ? 2U : 3U);
		emit(words, gadgets->immediate[0][sp_base ? 1 : 0]);
		emit(words, vector_operand_offset(spill.transfer) | base |
			displacement | fault_position(context));
		/* The program reads or writes a vector register. */
		context.program_accesses_fp_state = true;
	}
	return DecodeResult::decoded;
}

[[nodiscard]] std::uintptr_t memory_descriptor(unsigned operation,
	unsigned base, unsigned index, unsigned offset, unsigned mask) noexcept
{
	return register_offset(base) |
		(register_offset(12U + index) << 16) |
		(std::uintptr_t{offset} << 32) |
		(std::uintptr_t{base == 31U} << UML_GADGET_SME_BASE_SP_BIT) |
		(std::uintptr_t{mask} << UML_GADGET_SME_MASK_SHIFT) |
		(std::uintptr_t{operation} << UML_GADGET_SME_OPERATION_SHIFT);
}

/* A ZA or ZT0 access or zeroing: natively against resident host ZA in
 * streaming mode, through the register file otherwise. */
DecodeResult emit_za_memory(DecodeContext &context, selection::Gadget native,
	std::uintptr_t descriptor)
{
	const bool streaming = (context.options.svcr & svcr_sm) != 0U;
	emit(context.words, streaming ? native :
		selection::Gadget{uml_arm64_gadget_nsm_za_memory});
	emit(context.words, descriptor);
	emit(context.words, context.address);
	return DecodeResult::decoded;
}

} // namespace

DecodeResult decode_sme_pstate(DecodeContext &context)
{
	/* MSR SVCRSM, SVCRZA or SVCRSMZA, #imm (SMSTART and SMSTOP): CRm<3:1>
	 * names the bits, CRm<0> their value; other CRm<3:1> are reserved. */
	const auto instruction = context.instruction;
	const unsigned field = (instruction >> 9) & 7U;
	if (!sme_implemented(context) ||
	    (instruction & 0xffff'f0ffU) != 0xd503'407fU ||
	    field == 0U || field > 3U)
		return illegal_instruction(context);
	const auto mask = static_cast<std::uint8_t>(
		field == 1U ? svcr_sm : field == 2U ? svcr_za : svcr_mask);
	const auto value = static_cast<std::uint8_t>(
		(instruction & 0x100U) != 0U ? mask : 0U);
	return emit_svcr_write(context, mask, value);
}

DecodeResult decode_sme_system_register(DecodeContext &context)
{
	if (!sme_implemented(context))
		return illegal_instruction(context);
	const auto instruction = context.instruction;
	const unsigned rt = instruction & 31U;
	auto &words = context.words;
	switch (instruction & 0xffff'ffe0U) {
	case 0xd53b'4240U: /* MRS Xt, SVCR */
	case 0xd53b'd0a0U: /* MRS Xt, TPIDR2_EL0 */
		if (rt == 31U) {
			emit(words, selection::uml_arm64_gadget_nop);
			return DecodeResult::decoded;
		}
		emit(words, (instruction & 0xffff'ffe0U) == 0xd53b'4240U ?
			selection::Gadget{uml_arm64_gadget_sme_svcr_read} :
			selection::Gadget{uml_arm64_gadget_sme_tpidr2_read});
		emit(words, register_offset(rt));
		return DecodeResult::decoded;
	case 0xd51b'4240U: /* MSR SVCR, Xt: SM and ZA from Xt<1:0> */
		if (rt == 31U)
			return emit_svcr_write(context, svcr_mask, 0);
		emit(words, selection::Gadget{
			uml_arm64_gadget_sme_svcr_write_register});
		emit(words, register_offset(rt));
		emit(words, context.address);
		++context.compiled;
		return DecodeResult::terminal;
	case 0xd51b'd0a0U: /* MSR TPIDR2_EL0, Xt */
		if (rt == 31U) {
			emit(words, selection::Gadget{
				uml_arm64_gadget_sme_tpidr2_write_z});
			return DecodeResult::decoded;
		}
		emit(words, selection::Gadget{uml_arm64_gadget_sme_tpidr2_write_r});
		emit(words, register_offset(rt));
		return DecodeResult::decoded;
	default:
		return illegal_instruction(context);
	}
}

DecodeResult decode_sme_space(DecodeContext &context)
{
	const auto instruction = context.instruction;
	const auto &host = context.options.host_capabilities;
	/* The SM = 0 vector-length family (detail/sm0_vector_length.hpp), which
	 * an SME virtual CPU executes: a vector-length word as the base word of
	 * the same effect, an ADD, SUB, MOVZ or MOVN, which touch no vector
	 * state, and a spill word as the memory access it is. */
	if ((context.options.svcr & svcr_sm) == 0U &&
	    detail::implements_sm0_vector_length(host)) [[unlikely]] {
		if (const auto equivalent =
			    detail::sm0_vector_length_equivalent(instruction)) {
			if (*equivalent == detail::sm0_vector_length_nop) {
				emit(context.words, selection::uml_arm64_gadget_nop);
				return DecodeResult::decoded;
			}
			DecodeContext base{context};
			base.instruction = *equivalent;
			return decode_data_processing(base);
		}
		if (const auto spill = detail::sm0_spill_access(instruction))
			return emit_sm0_spill(context, *spill);
	}
	if (!detail::sme_encoding_space(instruction) || !implements_sme(host))
		return implementation_gap(context);
	/* The profile decides first: a word it lacks is UNDEFINED, and one
	 * whose PSTATE.SM or PSTATE.ZA requirement this mode does not meet
	 * takes the SME trap; Linux reports both as SIGILL. */
	const auto *row = detail::decode_sme(instruction,
		implemented_sme_features(host), host.sme_svl_bytes);
	if (row == nullptr ||
	    !detail::sme_mode_permits(*row,
		    (context.options.svcr & svcr_sm) != 0U,
		    (context.options.svcr & svcr_za) != 0U))
		return illegal_instruction(context);
	auto &words = context.words;
	const unsigned rd = instruction & 31U;
	const unsigned rn = (instruction >> 5) & 31U;
	/* RDVL, ADDVL and ADDPL reach here only in streaming mode, where the
	 * vector length is the SVL: they are RDSVL, ADDSVL and ADDSPL. */
	if ((instruction & 0xffff'f000U) == 0x04bf'5000U) {
		/* RDSVL and RDVL Xd, #imm */
		if (rd == 31U) {
			emit(words, selection::uml_arm64_gadget_nop);
			return DecodeResult::decoded;
		}
		emit(words, selection::Gadget{uml_arm64_gadget_sme_rdsvl});
		emit(words, register_offset(rd));
		emit(words, signed_immediate((instruction >> 5) & 63U, 6));
		return DecodeResult::decoded;
	}
	if ((instruction & 0xffa0'f000U) == 0x0420'5000U) {
		/* ADDSVL, ADDSPL, ADDVL and ADDPL Xd|SP, Xn|SP, #imm */
		const unsigned source = (instruction >> 16) & 31U;
		emit(words, (instruction & 0x0040'0000U) != 0U ?
			selection::Gadget{uml_arm64_gadget_sme_addspl} :
			selection::Gadget{uml_arm64_gadget_sme_addsvl});
		emit(words, register_offset(rd) | (register_offset(source) << 16));
		emit(words, signed_immediate((instruction >> 5) & 63U, 6));
		return DecodeResult::decoded;
	}
	if ((instruction & 0xffdf'9c10U) == 0xe100'0000U) {
		/* LDR and STR ZA[Wv, #imm], [Xn|SP, #imm, MUL VL] */
		const bool store = (instruction & 0x0020'0000U) != 0U;
		return emit_za_memory(context, store ?
			selection::Gadget{uml_arm64_gadget_sm_za_vector_store} :
			selection::Gadget{uml_arm64_gadget_sm_za_vector_load},
			memory_descriptor(store ? UML_GADGET_SME_ZA_STORE :
				UML_GADGET_SME_ZA_LOAD, rn,
				(instruction >> 13) & 3U, instruction & 15U, 0));
	}
	if ((instruction & 0xffdf'fc1fU) == 0xe11f'8000U) {
		/* LDR and STR ZT0, [Xn|SP] */
		const bool store = (instruction & 0x0020'0000U) != 0U;
		return emit_za_memory(context, store ?
			selection::Gadget{uml_arm64_gadget_sm_zt0_store} :
			selection::Gadget{uml_arm64_gadget_sm_zt0_load},
			memory_descriptor(store ? UML_GADGET_SME_ZT0_STORE :
				UML_GADGET_SME_ZT0_LOAD, rn, 0, 0, 0));
	}
	const bool streaming = (context.options.svcr & svcr_sm) != 0U;
	if ((instruction & 0xffff'ff00U) == 0xc008'0000U) {
		/* ZERO { mask } */
		const unsigned mask = instruction & 0xffU;
		if (streaming) {
			emit(words, reinterpret_cast<std::uintptr_t>(
				uml_arm64_gadget_sm_za_zero_table) +
				mask * za_zero_stride);
			return DecodeResult::decoded;
		}
		return emit_za_memory(context, nullptr, memory_descriptor(
			UML_GADGET_SME_ZA_ZERO, 0, 0, 0, mask));
	}
	if (instruction == 0xc048'0001U) {
		/* ZERO { ZT0 } */
		if (streaming) {
			emit(words, selection::Gadget{uml_arm64_gadget_sm_zt0_zero});
			return DecodeResult::decoded;
		}
		return emit_za_memory(context, nullptr, memory_descriptor(
			UML_GADGET_SME_ZT0_ZERO, 0, 0, 0, 0));
	}
	if (streaming) {
		if (const auto result = decode_streaming_memory(context, *row))
			return *result;
		if (const auto result = decode_streaming_sve(context, *row))
			return *result;
	}
	return implementation_gap(context);
}

namespace {

/* The twin rows by base address, for a binary search per instruction of a
 * streaming program. */
class StreamingTwinIndex final {
public:
	StreamingTwinIndex()
		: rows_{streaming_twins().begin(), streaming_twins().end()}
	{
		std::ranges::sort(rows_, {}, &StreamingTwin::base_begin);
	}

	[[nodiscard]] std::uintptr_t select(std::uintptr_t entry) const noexcept
	{
		const auto after = std::ranges::upper_bound(rows_, entry, {},
			&StreamingTwin::base_begin);
		if (after == rows_.begin())
			return entry;
		const auto &row = *(after - 1);
		return entry < row.base_end ?
			row.twin_begin + (entry - row.base_begin) : entry;
	}

private:
	std::vector<StreamingTwin> rows_;
};

} // namespace

std::span<const StreamingTwin> streaming_twins() noexcept
{
	return {uml_arm64_sme_twin_begin, uml_arm64_sme_twin_end};
}

/* Never destroyed, under the exit contract in docs/architecture.md, as the
 * FEAT_AFP twin index is. */
std::uintptr_t streaming_twin(std::uintptr_t entry)
{
	[[clang::no_destroy]] static const StreamingTwinIndex index;
	return index.select(entry);
}

/* A twin replaces its base entry even where the base is safe in streaming
 * mode, as the FPCR writes' twins do. */
bool select_streaming_entries(std::span<std::uintptr_t> emitted)
{
	const auto first = emitted[0];
	emitted[0] = streaming_twin(first);
	if (!streaming_entry(emitted[0]))
		return false;
	const bool prefix = first == reinterpret_cast<std::uintptr_t>(
		selection::uml_arm64_gadget_atomic_sp_alignment_read) ||
		first == reinterpret_cast<std::uintptr_t>(
			selection::uml_arm64_gadget_atomic_sp_alignment_write);
	if (!prefix)
		return true;
	if (emitted.size() < 2)
		return false;
	emitted[1] = streaming_twin(emitted[1]);
	return streaming_entry(emitted[1]);
}

bool streaming_entry(std::uintptr_t entry) noexcept
{
	for (const auto *row = uml_arm64_gadget_streaming_rows_begin;
	     row != uml_arm64_gadget_streaming_rows_end; ++row)
		if (entry >= row->begin && entry < row->end)
			return true;
	return false;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
