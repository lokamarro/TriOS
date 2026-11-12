// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_operand_encoding.hpp"
#include "gadget_selection_api.hpp"
#include "program.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/id_registers.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::arm64::detail {
struct SmeEncoding;
} // namespace uml::cpu::arm64::detail

namespace uml::cpu::arm64::gadgets::builder_detail {

struct DecodeContext final {
	std::uint64_t guest_address;
	std::uint64_t address;
	std::uint32_t instruction;
	/* The instructions after this one, for the decoders that fuse a pair into
	 * a single entry. Empty at the end of the window, so a decoder must check
	 * before reading it, and a fusing decoder owns advancing `compiled` past
	 * everything it consumed. */
	std::span<const std::uint32_t> following;
	std::size_t &compiled;
	bool &program_accesses_fp_state;
	ProgramWords &words;
	const Builder::LinkResolver &resolve_link;
	const Builder::IndirectLinkResolver &resolve_indirect_link;
	const BuildOptions &options;
	/* Where the suffix operands of inlined conditional branches are
	 * recorded, so `build` can fill them in once it knows how long the block
	 * turned out to be. */
	InlineSuffixes &suffixes;
};

/* The top eight bits of the operand of a transfer that carries no PC word:
 * the instruction's position in the block, which a fault reports and the C++
 * boundary turns back into the PC (see resolve_fault_position). `build`
 * bounds a block to fit. */
[[nodiscard]] inline std::uintptr_t fault_position(
	const DecodeContext &context) noexcept
{
	return static_cast<std::uintptr_t>(context.compiled) << 56;
}

/* Store `value` in the register at CpuState offset `offset`. A value that
 * fits 48 bits, or whose top sixteen bits are all ones, shares one stream word
 * with the offset, which sits in bits 63:48 over the value's low 48 bits;
 * any other value takes the offset and the value in a word each. */
inline void emit_constant_store(ProgramWords &words, std::uintptr_t offset,
				std::uint64_t value)
{
	constexpr std::uint64_t low_48 = (std::uint64_t{1} << 48) - 1;
	if ((value & ~low_48) == 0) {
		selection::emit(words,
			selection::uml_arm64_gadget_move_wide_packed_zero);
		selection::emit(words,
			static_cast<std::uintptr_t>(value) | (offset << 48));
	} else if ((value & ~low_48) == ~low_48) {
		selection::emit(words,
			selection::uml_arm64_gadget_move_wide_packed_ones);
		selection::emit(words, static_cast<std::uintptr_t>(
			value & low_48) | (offset << 48));
	} else {
		selection::emit(words,
			selection::uml_arm64_gadget_move_wide_constant);
		selection::emit(words, offset);
		selection::emit(words, static_cast<std::uintptr_t>(value));
	}
}

/* Emit the cell a direct control-flow gadget reads its successor through: this
 * executor's record for `target`. */
inline void emit_link(DecodeContext &context, std::uint64_t target)
{
	context.words.push_back(reinterpret_cast<std::uintptr_t>(
		context.resolve_link(target)));
}

/* The target of the B.cond, CBZ or CBNZ at `address`: its imm19, in words.
 *
 * This, emit_edges and emit_fused_compare_branch are forced inline because
 * they stand for code that was written out at each branch decoder. Translation
 * reaches them once per branch; a call would add to every one of those and
 * move the text behind them. */
[[nodiscard, gnu::always_inline]] inline std::uint64_t branch19_target(
	std::uint64_t address, std::uint32_t word) noexcept
{
	return static_cast<std::uint64_t>(static_cast<std::int64_t>(address) +
		selection::sign_extend(static_cast<std::uint64_t>(
			(word >> 5) & 0x7ffffU) << 2, 21));
}

/* The two edges of a conditional transfer, the taken one first: the cells of
 * their blocks when the builder links, else their guest addresses. */
[[gnu::always_inline]] inline void emit_edges(
	DecodeContext &context, std::uint64_t target, std::uint64_t fallthrough)
{
	if (context.resolve_link) {
		emit_link(context, target);
		emit_link(context, fallthrough);
	} else {
		selection::emit(context.words, target);
		selection::emit(context.words, fallthrough);
	}
}

/* Emit the cell an indirect branch reads its trained targets through. This one
 * names the branch site rather than a target, because the site is what owns the
 * trained cells. */
inline void emit_indirect_link(DecodeContext &context, std::uint64_t source,
			       bool is_return = false)
{
	auto *site = context.resolve_indirect_link(source);
#if UML_GADGET_EXECUTION_HISTOGRAM
	if (site != nullptr)
		site->kind = is_return ? 1 : 0;
#else
	(void)is_return;
#endif
	context.words.push_back(reinterpret_cast<std::uintptr_t>(site));
}

/* Continue translating the fall-through of a conditional branch rather than
 * ending the block at it. A block otherwise ends at its first branch, and
 * conditional branches are the only large category of block ends whose
 * continuation keeps the guest addresses contiguous; see G162.
 *
 * The taken edge keeps its link. The word after it holds the count of
 * instructions translated after this one, which the taken path gives back to
 * the precharged total before it leaves. `build` fills that in; here it is
 * only reserved and recorded.
 *
 * Returns false when the block must end at this branch after all, in which case
 * the caller emits its ordinary terminal form and nothing has been written.
 */
[[nodiscard]] inline bool emit_inlined_conditional(
	DecodeContext &context, selection::Gadget gadget, std::uint64_t target,
	std::size_t retires = 1)
{
	if (gadget == nullptr || context.suffixes.full())
		return false;
	selection::emit(context.words, gadget);
	emit_link(context, target);
	context.suffixes.add(context.words.size(), context.compiled + retires);
	context.words.push_back(0);
	return true;
}

/* Pack an inline suffix into a zero high-bit range of an operand. build()
 * caps a block at 2^16 instructions, so a suffix needs at most sixteen bits,
 * which a shift of 48 or less leaves free above the operand. */
[[nodiscard]] inline bool emit_inlined_packed_compare_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uint64_t target, std::uint8_t suffix_shift,
	std::size_t retires = 1)
{
	if (gadget == nullptr || suffix_shift == 0 || suffix_shift > 48 ||
	    (operand >> suffix_shift) != 0 ||
	    context.suffixes.full())
		return false;
	selection::emit(context.words, gadget);
	context.suffixes.add(context.words.size(),
		context.compiled + retires, suffix_shift);
	selection::emit(context.words, operand);
	emit_link(context, target);
	return true;
}

[[nodiscard]] inline bool emit_inlined_compare_zero_branch(
	DecodeContext &context, selection::Gadget gadget,
	std::uintptr_t operand, std::uint64_t target, std::size_t retires = 1)
{
	return emit_inlined_packed_compare_branch(
		context, gadget, operand, target, 16, retires);
}


enum class DecodeResult : std::uint8_t {
	decoded,
	terminal,
	implementation_gap,
};

/* End the block at an encoding the virtual CPU does not implement: the
 * illegal terminal reports it at this instruction, which retires. */
[[nodiscard]] inline DecodeResult illegal_instruction(DecodeContext &context)
{
	selection::emit(context.words, selection::uml_arm64_gadget_illegal);
	selection::emit(context.words, context.address);
	++context.compiled;
	return DecodeResult::terminal;
}

/* A compare and the B.cond after it, translated as one entry. `branch` is the
 * B.cond word and `operand` the compare's operand, with room above
 * `suffix_shift` for an inline suffix. A non-null `inline_entry` translates the
 * fall-through of the pair behind it, exactly as for an unfused branch; the
 * loop then advances past the compare itself and the pair's second instruction
 * is counted here. Otherwise `fused` ends the block, with both instructions
 * retired. */
[[nodiscard, gnu::always_inline]] inline DecodeResult
emit_fused_compare_branch(
	DecodeContext &context, std::uint32_t branch, selection::Gadget fused,
	selection::Gadget inline_entry, std::uintptr_t operand,
	std::uint8_t suffix_shift)
{
	const auto branch_address = context.address + sizeof(std::uint32_t);
	const auto target = branch19_target(branch_address, branch);
	if (emit_inlined_packed_compare_branch(context, inline_entry, operand,
					       target, suffix_shift, 2)) {
		++context.compiled;
		return DecodeResult::decoded;
	}
	selection::emit(context.words, fused);
	selection::emit(context.words, operand);
	emit_edges(context, target, branch_address + sizeof(std::uint32_t));
	context.compiled += 2;
	return DecodeResult::terminal;
}

[[nodiscard]] DecodeResult implementation_gap(DecodeContext &context);
[[nodiscard]] DecodeResult decode_branch_system(DecodeContext &context);
[[nodiscard]] DecodeResult decode_data_processing(DecodeContext &context);
[[nodiscard]] DecodeResult decode_floating_simd(DecodeContext &context);
[[nodiscard]] DecodeResult decode_load_store(DecodeContext &context);
[[nodiscard]] DecodeResult decode_exception(DecodeContext &context);

/* The SME instructions (program_builder_sme.cpp), which decode_branch_system
 * reaches only where it would otherwise end the block: an MSR (immediate) no
 * base PSTATE field claims, a system register it does not know, and a word
 * no other decoder matched. Without SME on the virtual CPU each ends the
 * block as that path always has. */
[[nodiscard]] DecodeResult decode_sme_pstate(DecodeContext &context);
[[nodiscard]] DecodeResult decode_sme_system_register(DecodeContext &context);
[[nodiscard]] DecodeResult decode_sme_space(DecodeContext &context);

/* The generated register-only streaming forms
 * (program_builder_streaming.cpp). decode_streaming_sve() translates a word
 * of an SVE, SVE2 or SME row for SVCR.SM = 1, or returns nothing for a row
 * without a generated form; decode_streaming_fallback() translates a word
 * whose base entries streaming mode cannot select: an FP&SIMD class legal in
 * streaming mode, or else the illegal exit. */
[[nodiscard]] std::optional<DecodeResult> decode_streaming_sve(
	DecodeContext &context, const detail::SmeEncoding &row);
/* The vector memory gadgets: decode_streaming_memory() translates a word of
 * an SVE or SME load, store or prefetch row for SVCR.SM = 1, or returns
 * nothing for a row that is not one or has no gadget; the fallback also
 * takes the SIMD&FP loads and stores streaming mode keeps. */
[[nodiscard]] std::optional<DecodeResult> decode_streaming_memory(
	DecodeContext &context, const detail::SmeEncoding &row);
[[nodiscard]] DecodeResult decode_streaming_fallback(DecodeContext &context);

/* Whether a program translated for SVCR.SM = 1 may select `entry`: an entry
 * of the SME part's streaming half or of a base row engine/sme.inc lists. */
[[nodiscard]] bool streaming_entry(std::uintptr_t entry) noexcept;

/* One row of the streaming twin table engine/sme.inc emits, laid out as the
 * FEAT_AFP twin table is: entries [base_begin, base_end) run as the entries
 * at the same offsets from twin_begin in a program translated for
 * SVCR.SM = 1. */
struct StreamingTwin final {
	std::uintptr_t base_begin;
	std::uintptr_t base_end;
	std::uintptr_t twin_begin;
	std::uintptr_t twin_end;
};
static_assert(sizeof(StreamingTwin) == 32);

[[nodiscard]] std::span<const StreamingTwin> streaming_twins() noexcept;

/* The entry a program translated for SVCR.SM = 1 selects in place of base
 * entry `entry`: its streaming twin, or `entry` itself when it has none. */
[[nodiscard]] std::uintptr_t streaming_twin(std::uintptr_t entry);

/* Selects the streaming forms of the entries one decode emitted for a
 * program translated for SVCR.SM = 1: a base entry that calls helpers becomes
 * its twin, and so does the entry an SP-alignment prefix dispatches after its
 * check. Returns whether every entry may then be selected there. */
[[nodiscard]] bool select_streaming_entries(std::span<std::uintptr_t> emitted);

} // namespace uml::cpu::arm64::gadgets::builder_detail
