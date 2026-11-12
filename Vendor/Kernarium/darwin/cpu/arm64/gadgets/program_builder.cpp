// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "afp_twin.hpp"
#include "gadget_selection.hpp"
#include "execution_histogram.hpp"
#include "scalar_forwarding.hpp"

#include "cpu/arm64/floating_controls.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

/* Builder::build() below calls one of these five per guest instruction it
 * translates, and each of them calls on into the gadget selection tables.
 * Before the engine split that whole decode -> select -> emit chain lived in
 * one translation unit and the compiler inlined it freely; separating the
 * families into their own objects put hard call boundaries across the hottest
 * loop in translation and cost measurable score. Interprocedural optimization
 * recovered part of it but left single-core Clang, the most translation-heavy
 * workload, untouched, because ThinLTO imports across modules on a cost model
 * that declines functions this large. Composing them textually here is not a
 * shortcut around that decision: it is the only way to make the inliner see the
 * whole chain rather than leave it to a heuristic that has been measured to
 * decline. Each fragment still owns exactly one decode family. */
/* .ipp rather than .inc: every .inc in this tree is an assembly fragment of the
 * gadget bank, and editors do not treat .inc as C++. */
#include "program_builder_branch_system_impl.ipp"
#include "program_builder_data_processing_impl.ipp"
#include "program_builder_exception_impl.ipp"
#include "program_builder_floating_simd_impl.ipp"
#include "program_builder_load_store_impl.ipp"

namespace uml::cpu::arm64::gadgets {

using namespace selection;
using namespace builder_detail;

DecodeResult builder_detail::implementation_gap(DecodeContext &context)
{
	/* Keep a translated prefix executable. The terminal gadget reports the
	 * first unsupported allocation without charging it as a retired
	 * instruction; instruction_count deliberately remains the number of
	 * successfully translated words.
	 */
	emit(context.words, uml_arm64_gadget_illegal);
	emit(context.words, context.address);
	return DecodeResult::implementation_gap;
}

namespace {

/* Fill in the reserved suffix operand of each inlined conditional branch, now
 * that the block's translated length is final. A taken branch subtracts this
 * from the count its block was precharged, so it is the instructions after the
 * branch itself. */
void resolve_inline_suffixes(ProgramWords &words,
			     const InlineSuffixes &suffixes,
			     std::size_t compiled) noexcept
{
	for (std::uint32_t index = 0; index < suffixes.count; ++index) {
		const auto translated = suffixes.compiled[index];
		const auto suffix = translated >= compiled ?
			0 : compiled - translated;
		if (suffixes.shift[index] == 0)
			words[suffixes.word[index]] = suffix;
		else
			words[suffixes.word[index]] |=
				suffix << suffixes.shift[index];
	}
}

} // namespace

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	ProgramWords words{storage};
	InlineSuffixes suffixes{};
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	/* A transfer without a PC word names its instruction by an 8-bit
	 * position in the block, which also keeps every packed inline suffix
	 * within sixteen bits. */
	const auto window = instructions.first(std::min(instructions.size(),
		fault_position_instructions));
	/* A program for SVCR.SM = 1 selects only entries that are safe in
	 * streaming mode; forwarding contracts do not reach into them, and its
	 * executor loads the FP state before it enters, so it needs no FP link
	 * entry. */
	const bool streaming = (options.svcr & svcr_sm) != 0U;
	if (streaming) [[unlikely]] {
		options.scalar_forwarding = false;
		options.vector_forwarding = false;
	}
	const auto finish = [&](std::size_t retired) {
		resolve_inline_suffixes(words, suffixes, retired);
		return Program{guest_address, retired, std::move(words),
			program_accesses_fp_state && !streaming};
	};
#if UML_GADGET_EXECUTION_HISTOGRAM
	const void *previous_gadget = nullptr;
#endif
	std::uint16_t previous_scalar = scalar_no_register;
	std::uint16_t previous_vector = scalar_no_register;
	/* FIZ, AH and NEP are guest state only where the virtual CPU implements
	 * FEAT_AFP; there every entry that would ignore them runs as its twin,
	 * which carries no forwarding contract. */
	const bool afp = implements_afp(options.host_capabilities);
	/* Indexed rather than ranged, because a decoder that fuses a pair needs to
	 * see what follows the instruction it was handed. `compiled` is the loop
	 * position as well as the retired count, and a fusing decoder advances it
	 * past both instructions itself. */
	while (compiled < window.size()) {
		const auto instruction = window[compiled];
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
#if UML_GADGET_EXECUTION_HISTOGRAM
		const auto counter_word = words.size();
		emit(words, diagnostics::uml_arm64_gadget_execution_count);
		emit(words, std::uintptr_t{sizeof(std::uint64_t)});
		emit(words, std::uintptr_t{1});
#endif
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {finish(compiled), true, address, instruction};
		}

		const auto compiled_before = compiled;
		DecodeContext context{
			guest_address,
			address,
			instruction,
			window.subspan(compiled + 1),
			compiled,
			program_accesses_fp_state,
			words,
			resolve_link,
			resolve_indirect_link,
			options,
			suffixes,
		};
		const auto emitted_start = words.size();
		DecodeResult decoded = DecodeResult::decoded;
		switch (production_decode_starts[instruction >> 24]) {
		case ProductionDecodeStart::branch_system:
			decoded = decode_branch_system(context);
			break;
		case ProductionDecodeStart::data_processing_immediate:
		case ProductionDecodeStart::data_processing_register:
			decoded = decode_data_processing(context);
			break;
		case ProductionDecodeStart::data_processing_pointer_auth_overlap:
			decoded = pointer_auth_lr_unavailable(instruction) ?
				decode_branch_system(context) :
				decode_data_processing(context);
			break;
		case ProductionDecodeStart::floating_simd:
			decoded = decode_floating_simd(context);
			break;
		case ProductionDecodeStart::load_store:
			decoded = decode_load_store(context);
			break;
		case ProductionDecodeStart::exception:
			decoded = decode_exception(context);
			break;
		}
		if (afp && words.size() > emitted_start)
			words[emitted_start] = afp_twin(words[emitted_start]);
		const auto current_vector = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			vector_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto vector_mask = current_vector.source_mask(previous_vector);
		previous_vector = current_vector.destination;
		// The selected assembly entry owns its access description. Do not
		// infer it again from opcode bits: aliases and fused groups differ.
		const auto current_scalar = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			scalar_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto source_mask = current_scalar.source_mask(previous_scalar);
		if (source_mask != 0 && options.scalar_forwarding)
			words[emitted_start] = current_scalar.contract->entries[source_mask];
		previous_scalar = current_scalar.destination;
		if (vector_mask != 0 && options.vector_forwarding)
			words[emitted_start] = current_vector.contract->entries[vector_mask];
		if (streaming && words.size() > emitted_start &&
		    !select_streaming_entries(std::span{words.data(),
			    words.size()}.subspan(emitted_start))) [[unlikely]] {
			/* The base entries are unsafe in streaming mode: an
			 * FP&SIMD class legal there takes its generated form, and
			 * any other word ends the block at the illegal exit, as
			 * an unimplemented word does. */
			words.truncate(emitted_start);
			while (suffixes.count != 0 &&
			       suffixes.word[suffixes.count - 1] >= emitted_start)
				--suffixes.count;
			compiled = compiled_before;
			decoded = decode_streaming_fallback(context);
		}
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The entry is final only after forwarding selection above. */
		if (words.size() > emitted_start) {
			words[counter_word + 1] = diagnostics::execution_histogram_slot(
				previous_gadget,
				reinterpret_cast<const void *>(words[emitted_start]));
			words[counter_word + 2] = std::max<std::size_t>(1,
				compiled - compiled_before +
				(decoded == DecodeResult::decoded));
		}
		/* The first word a decode emits is its gadget pointer, which the
		 * execution histogram pairs with its predecessor. */
		if (words.size() > emitted_start)
			previous_gadget = reinterpret_cast<const void *>(
				words[emitted_start]);
#endif
		if (decoded != DecodeResult::decoded)
			return {finish(compiled), decoded == DecodeResult::terminal,
				address, instruction};
		++compiled;
	}
	const auto fallthrough = guest_address + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	return {finish(compiled), true, fallthrough, 0};
}

} // namespace uml::cpu::arm64::gadgets
