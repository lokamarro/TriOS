// SPDX-License-Identifier: GPL-2.0-or-later
#include "translator.hpp"

#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/decode_support.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/gadgets/a32_layout.h"
#include "cpu/arm64/gadgets/gadget_symbols.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

/* The A64 engine's gadgets the AArch32 part shares: control flow, which
 * carries a key where an A64 stream carries a PC; and the exclusive-access
 * family, whose host monitor and forward-progress rules are the engine's
 * (system_atomic.inc). */
extern "C" {
void uml_arm64_gadget_branch_linked();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_clear_exclusive();
void uml_arm64_gadget_exclusive_load_relaxed_b_r();
void uml_arm64_gadget_exclusive_load_relaxed_h_r();
void uml_arm64_gadget_exclusive_load_relaxed_w_r();
void uml_arm64_gadget_exclusive_store_relaxed_b_rr();
void uml_arm64_gadget_exclusive_store_relaxed_h_rr();
void uml_arm64_gadget_exclusive_store_relaxed_w_rr();
void uml_arm64_gadget_exclusive_pair_load_relaxed_w_rr();
void uml_arm64_gadget_exclusive_pair_store_relaxed_w_rrr();
void uml_arm64_gadget_exclusive_load_acquire_b_r();
void uml_arm64_gadget_exclusive_load_acquire_h_r();
void uml_arm64_gadget_exclusive_load_acquire_w_r();
void uml_arm64_gadget_exclusive_store_release_b_rr();
void uml_arm64_gadget_exclusive_store_release_h_rr();
void uml_arm64_gadget_exclusive_store_release_w_rr();
void uml_arm64_gadget_exclusive_pair_load_acquire_w_rr();
void uml_arm64_gadget_exclusive_pair_store_release_w_rrr();
void uml_arm64_gadget_ordered_load_acquire_b_r();
void uml_arm64_gadget_ordered_load_acquire_h_r();
void uml_arm64_gadget_ordered_load_acquire_w_r();
void uml_arm64_gadget_ordered_store_release_b_r();
void uml_arm64_gadget_ordered_store_release_h_r();
void uml_arm64_gadget_ordered_store_release_w_r();
}

namespace uml::cpu::arm64::gadgets::a32 {
namespace {

using namespace aarch32;

/* The conditional branches, fused compare-and-branch pairs and barriers are
 * the A64 bank's, which gadget_symbols.hpp lists. */
constexpr std::array<void (*)(), 14> conditional_inline{
	UML_ARM64_CONDITION_GADGETS(selection::uml_arm64_gadget_branch_, _inline)};
/* The AArch64 bank's fused CMP and B.cond, in the 32-bit form, which read
 * the same CpuState slots an AArch32 register is in: a compare followed at
 * once by a conditional branch whose fall-through is translated inline. */
constexpr std::array<void (*)(), 14> compare_register_branch_inline{
	UML_ARM64_CONDITION_GADGETS(
		selection::uml_arm64_gadget_compare_register_branch_w_, _inline)};
constexpr std::array<void (*)(), 14> compare_immediate_branch_inline{
	UML_ARM64_CONDITION_GADGETS(
		selection::uml_arm64_gadget_compare_immediate_branch_w_, _inline)};
constexpr std::array<void (*)(), 14> conditional_plain{
	UML_ARM64_CONDITION_GADGETS(selection::uml_arm64_gadget_branch_, )};
constexpr std::array<void (*)(), 16> data_barrier{
	UML_ARM64_BARRIER_GADGETS(selection::uml_arm64_gadget_dmb_)};
constexpr std::array<void (*)(), 16> synchronization_barrier{
	UML_ARM64_BARRIER_GADGETS(selection::uml_arm64_gadget_dsb_)};

/* The data-processing operations, as a32_forms.hpp numbers them. */
enum Op : unsigned {
	op_and, op_eor, op_sub, op_rsb, op_add, op_adc, op_sbc, op_rsc, op_tst,
	op_teq, op_cmp, op_cmn, op_orr, op_mov, op_bic, op_mvn, op_orn,
};

enum Shape : unsigned {
	shape_imm, shape_imc, shape_reg, shape_lsl, shape_lsr, shape_asr,
	shape_ror, shape_rrx, shape_rlsl, shape_rlsr, shape_rasr, shape_rror,
};

[[nodiscard]] constexpr bool is_logical(unsigned op) noexcept
{
	return op == op_and || op == op_eor || op == op_tst || op == op_teq ||
	       op == op_orr || op == op_mov || op == op_bic || op == op_mvn ||
	       op == op_orn;
}

[[nodiscard]] constexpr bool is_compare(unsigned op) noexcept
{
	return op == op_tst || op == op_teq || op == op_cmp || op == op_cmn;
}

/* The forms whose immediate the decoder leaves as the raw 12-bit field. */
[[nodiscard]] constexpr bool raw_immediate(unsigned op) noexcept
{
	return op == op_and || op == op_eor || op == op_bic || op == op_orr ||
	       op == op_orn || op == op_tst || op == op_teq || op == op_mvn;
}

[[nodiscard]] std::uint32_t mask_of_width(unsigned width) noexcept
{
	return width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << width) - 1U;
}

/* An indirect site that never matches, for the table branches of an
 * uncached single instruction: its miss leaves with the target as the PC. */
IndirectLink &dummy_site() noexcept
{
	static IndirectLink site = [] {
		IndirectLink empty;
		for (auto &entry : empty.entries)
			entry = {~std::uint64_t{}, nullptr};
		return empty;
	}();
	return site;
}

} // namespace

namespace detail {

void Translator::close_run()
{
	if (!run_active_)
		return;
	run_active_ = false;
	const std::size_t words = words_.size() - (run_operand_ + 1);
	if (words < kSkipWordsMin || words > kSkipWordsMax) {
		words_[run_operand_] = words * sizeof(std::uintptr_t);
		return;
	}
	/* A run of a length a skip gadget holds takes that gadget, and its
	 * operand leaves the stream: every word and recorded position after it
	 * moves down one. */
	words_[run_operand_ - 1] =
		gadget(kSkipBy[run_cond_][words - kSkipWordsMin]);
	auto *data = words_.data();
	std::copy(data + run_operand_ + 1, data + words_.size(),
		  data + run_operand_);
	words_.truncate(words_.size() - 1U);
	const auto moved = [this](std::size_t &index) {
		if (index != ~std::size_t{} && index > run_operand_)
			--index;
	};
	for (auto &patch : patches_)
		moved(patch.index);
	moved(compare_.end);
	moved(movw_.end);
	moved(vfp_compare_.end);
	moved(address_add_.start);
	moved(address_add_.end);
}

void Translator::begin_instruction(unsigned cond)
{
	if (cond >= 14) {
		if (run_active_)
			close_run();
		return;
	}
	if (run_active_ && run_cond_ == cond && !run_dirty_)
		return;
	close_run();
	static constexpr std::array<G, 14> skips{
		G::skip_eq, G::skip_ne, G::skip_cs, G::skip_cc, G::skip_mi,
		G::skip_pl, G::skip_vs, G::skip_vc, G::skip_hi, G::skip_ls,
		G::skip_ge, G::skip_lt, G::skip_gt, G::skip_le};
	push(skips[cond]);
	run_operand_ = words_.size();
	push(0);
	run_active_ = true;
	run_cond_ = cond;
	run_dirty_ = false;
}

void Translator::credit()
{
	if (!run_active_)
		return;
	push(G::credit);
	patches_.push_back({words_.size(), compiled_ + 1, 0});
	push(0);
}

std::uintptr_t Translator::source(int number, std::uintptr_t slot)
{
	if (number != 15)
		return static_cast<std::uintptr_t>(number) * 8U;
	set_scratch(slot, pc_value());
	return slot;
}

void Translator::set_scratch(std::uintptr_t slot, std::uint32_t value)
{
	push(G::set_scratch);
	push(slot);
	push(value);
}

/* The branch to the PC an instruction wrote to a register slot. */
void Translator::emit_indirect(std::uintptr_t register_offset,
			       bool interwork, bool returns)
{
	credit();
	const bool cached = !!indirects_;
	if (interwork) {
		push(cached ? (returns ? G::bx_return_cached : G::bx_cached)
			    : G::bx);
	} else {
		push(cached ? (returns ? G::bw_return_cached : G::bw_cached)
			    : G::bw);
	}
	push(register_offset);
	if (cached)
		push(reinterpret_cast<std::uintptr_t>(indirects_(key_)));
}

void Translator::emit_direct_exit(std::uint64_t target)
{
	credit();
	if (linked()) {
		push_external(uml_arm64_gadget_branch_linked);
		push_link(target);
	} else {
		push_external(uml_arm64_gadget_branch);
		push(target);
	}
}

void Translator::emit_fallthrough()
{
	close_run();
	const auto next = input_.key + 4U * unit_;
	if (it_.active() && !input_.single) {
		/* Ended inside an IT block: record where, and leave through the
		 * executor, whose next entry runs the rest unlinked. */
		push(G::set_itstate);
		push(it_.bits());
		push_external(uml_arm64_gadget_interrupt);
		push(next);
		return;
	}
	if (linked()) {
		/* Unlinked and IT exits leave anyway, and the next entry sees
		 * any code change; a linked one checks after an ISB. */
		if (synchronize_) {
			push(G::code_sequence_check);
			push(next);
		}
		push_external(uml_arm64_gadget_branch_linked);
		push_link(next);
	} else {
		push_external(uml_arm64_gadget_interrupt);
		push(next);
	}
}

void Translator::emit_illegal()
{
	push_external(uml_arm64_gadget_illegal);
	push(key_);
}

/* An instruction the profile cannot run ends the block at the illegal
 * exit. */
void Translator::finish_stop(bool, std::uint32_t word)
{
	close_run();
	push_external(uml_arm64_gadget_illegal);
	push(key_);
	complete_ = false;
	stop_address_ = key_;
	stop_instruction_ = word;
}

/* ------------------------------------------------------------------ DP */

Translator::Result Translator::emit_dp_values(const Instruction &in,
	unsigned op, bool s, unsigned shape_class, std::uintptr_t rd,
	std::uintptr_t rn, std::uintptr_t rm, std::uintptr_t rs,
	std::uint32_t immediate, std::optional<unsigned> carry, bool in_place)
{
	const auto &o = in.operands;
	const bool logical = is_logical(op);
	const bool compare = is_compare(op);
	const bool sets = s || compare;
	unsigned shape = shape_imm;
	std::uint64_t word = rd | rn << 8;
	if (shape_class == 0) {
		if (logical && sets && carry) {
			shape = shape_imc;
			word |= std::uint64_t{*carry} << 16;
		}
		word |= std::uint64_t{immediate} << 32;
	} else {
		word |= rm << 16;
		if (shape_class == 1) {
			const auto amount = static_cast<unsigned>(o.shift_n);
			switch (o.shift_t) {
			case SRType::LSL:
				if (amount == 0) {
					shape = shape_reg;
				} else {
					shape = shape_lsl;
					word |= std::uint64_t{amount |
						(32U - amount) << 6} << 32;
				}
				break;
			case SRType::LSR:
				shape = shape_lsr;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::ASR:
				shape = shape_asr;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::ROR:
				shape = shape_ror;
				word |= std::uint64_t{amount |
					(amount - 1U) << 6} << 32;
				break;
			case SRType::RRX:
				shape = shape_rrx;
				break;
			}
		} else {
			word |= rs << 24;
			switch (o.shift_t) {
			case SRType::LSL: shape = shape_rlsl; break;
			case SRType::LSR: shape = shape_rlsr; break;
			case SRType::ASR: shape = shape_rasr; break;
			default: shape = shape_rror; break;
			}
		}
	}
	/* A conditional instruction that sets no flags, as a select of the
	 * result and the old value (run() opened no skip for it). A shape
	 * without a select gadget goes behind a skip after all: nothing of
	 * this instruction is in the stream yet but scratch values. */
	if (select_ < 14) {
		const unsigned cond = select_;
		select_ = 14;
		const G which = sets || rd >= 15U * 8U ? G::count :
			shape == shape_imm ? kDpSelect[op][0] :
			shape == shape_reg ? kDpSelect[op][1] : G::count;
		if (which != G::count) {
			push(which);
			push(condition_table(cond) | std::uint64_t{rd} << 16 |
			     std::uint64_t{rn} << 24 |
			     (shape == shape_imm ? std::uint64_t{immediate} :
						   std::uint64_t{rm}) << 32);
			return {};
		}
		begin_instruction(cond);
	}
	/* An unconditional ADD or SUB of an immediate between registers that
	 * sets no flags may become the base of a VLD1 or VST1 copy. */
	const bool address = shape == shape_imm &&
		(op == op_add || op == op_sub) && !sets && in.cond == 14 &&
		!in.in_it && rd < 15U * 8U && rn < 15U * 8U;
	const std::size_t start = words_.size();
	/* ADD or SUB of an immediate to a register in place: the entry with
	 * the register baked. R15 and the scratch slots are not registers. */
	if (in_place && shape == shape_imm && (op == op_add || op == op_sub) &&
	    rd == rn && rd < 15U * 8U) {
		push(kDpInplace[op == op_sub ? 1 : 0][sets ? 1 : 0][rd / 8U]);
		push(word);
		if (address)
			address_add_ = {start, words_.size(), rd, rn,
					op == op_sub ? 0U - immediate : immediate};
		return {false, sets};
	}
	const G which = kDp[op][sets ? 1 : 0][shape];
	if (which == G::count)
		return {};
	push(which);
	push(word);
	if (address)
		address_add_ = {start, words_.size(), rd, rn,
				op == op_sub ? 0U - immediate : immediate};
	/* A plain CMP is the first half of a fused compare and branch: of a
	 * register, or of an immediate that fits the fused entry's field. */
	if (op == op_cmp && in.cond == 14 && !in.in_it) {
		if (shape == shape_reg) {
			compare_ = {words_.size(), false, rn << 16 | rm << 32};
		} else if (shape == shape_imm && immediate < (1U << 24)) {
			compare_ = {words_.size(), true,
				    (rn / 8U) << 8 | std::uint64_t{immediate} << 16};
		}
	}
	return {false, sets};
}

Translator::Result Translator::emit_mov_constant(const Instruction &in, int d,
	std::uint32_t value)
{
	const bool to_pc = d == 15;
	const std::uintptr_t rd =
		to_pc ? scratch0 : static_cast<std::uintptr_t>(d) * 8U;
	(void)emit_dp_values(in, op_mov, false, 0, rd, 0, 0, 0, value,
			     std::nullopt);
	if (to_pc)
		emit_indirect(scratch0, !thumb_, false);
	return {to_pc, false};
}

Translator::Result Translator::emit_dp(const Instruction &in,
				       const FormPlan &plan)
{
	const auto &o = in.operands;
	unsigned op = plan.sub;
	unsigned shape_class = plan.aux & 3U;
	const bool s = o.setflags || is_compare(op);
	const bool unary = op == op_mov || op == op_mvn;
	/* The SP forms name no Rn: the first operand is R13. */
	int first = (plan.aux & 4U) != 0 ? 13 : o.n;
	std::uint32_t immediate = 0;
	/* PC as an unshifted register operand is a constant, so the
	 * instruction is the immediate form of its operation, which needs no
	 * scratch slot holding the PC value: PC as the second operand of any
	 * operation, and as the first of one that commutes or, for SUB and
	 * RSB, of the other. An unshifted register keeps C, as an immediate
	 * without a carry does. Such a sum is an address: an indexed load
	 * wrote the literal it adds to and indexed loads consume it, so it
	 * takes the indexed entry even in place, because rename forwards a
	 * CpuState hop only between accesses of one addressing form (G199). */
	bool folded = false;
	if (shape_class == 1 && o.shift_t == SRType::LSL && o.shift_n == 0) {
		if (o.m == 15 && (unary || first != 15)) {
			folded = true;
		} else if (!unary && first == 15 && o.m != 15) {
			switch (op) {
			case op_add: case op_adc: case op_and: case op_eor:
			case op_orr: case op_tst: case op_teq: case op_cmn:
				folded = true;
				break;
			case op_sub: op = op_rsb; folded = true; break;
			case op_rsb: op = op_sub; folded = true; break;
			default: break;
			}
			if (folded)
				first = o.m;
		}
		if (folded) {
			shape_class = 0;
			immediate = pc_value();
		}
	}
	std::uintptr_t rn = 0;
	std::uintptr_t rm = 0;
	std::uintptr_t rs = 0;
	if (!unary)
		rn = source(first, scratch0);
	if (shape_class != 0)
		rm = source(o.m, scratch1);
	if (shape_class == 2)
		rs = static_cast<std::uintptr_t>(o.s) * 8U;
	std::optional<unsigned> carry;
	if (shape_class == 0 && !folded) {
		if (raw_immediate(op)) {
			const auto expanded = o.a32 ?
				aarch32::detail::a32_expand_imm_c(o.imm) :
				aarch32::detail::t32_expand_imm_c(o.imm);
			immediate = static_cast<std::uint32_t>(expanded.imm32);
			if (!expanded.carry_in)
				carry = static_cast<unsigned>(expanded.carry);
		} else {
			immediate = static_cast<std::uint32_t>(o.imm32);
			if (is_logical(op) && !o.carry_flags)
				carry = o.carry;
		}
	}
	const bool to_pc = !is_compare(op) && o.d == 15;
	const std::uintptr_t rd = is_compare(op) ? 0 :
		to_pc ? scratch0 : static_cast<std::uintptr_t>(o.d) * 8U;
	const auto result = emit_dp_values(in, op, s, shape_class, rd, rn, rm,
					   rs, immediate, carry, !folded);
	if (to_pc)
		emit_indirect(scratch0, !thumb_, false);
	return {to_pc, result.flags};
}

/* ------------------------------------------------------------ multiply */

Translator::Result Translator::emit_multiply(const Instruction &in,
					     const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto m = static_cast<std::uint64_t>(o.m) * 8U;
	const auto a = static_cast<std::uint64_t>(o.a) * 8U;
	const auto lo = static_cast<std::uint64_t>(o.dLo) * 8U;
	const auto hi = static_cast<std::uint64_t>(o.dHi) * 8U;
	switch (plan.family) {
	case Family::mul: {
		const bool s = o.setflags;
		G which;
		if (plan.sub == 0)
			which = s ? G::muls : G::mul;
		else if (plan.sub == 1)
			which = s ? G::mlas : G::mla;
		else
			which = G::mls;
		push(which);
		push(d | n << 8 | m << 16 | a << 24);
		return {false, s};
	}
	case Family::mull: {
		static constexpr std::array<G, 10> table{
			G::umull, G::umulls, G::smull, G::smulls, G::umlal,
			G::umlals, G::smlal, G::smlals, G::umaal, G::umaal};
		const bool s = o.setflags && plan.sub != 4;
		push(table[plan.sub * 2U + (s ? 1U : 0U)]);
		push(lo | hi << 8 | n << 16 | m << 24);
		return {false, s};
	}
	case Family::mulh: {
		static constexpr std::array<G, 6> table{
			G::smmul, G::smmulr, G::smmla, G::smmlar, G::smmls,
			G::smmlsr};
		push(table[plan.sub]);
		push(d | n << 8 | m << 16 | a << 24);
		return {};
	}
	case Family::mulxy: {
		static constexpr std::array<G, 16> table{
			G::smulbb, G::smulbt, G::smultb, G::smultt, G::smlabb,
			G::smlabt, G::smlatb, G::smlatt, G::smulwb, G::smulwt,
			G::smlawb, G::smlawt, G::smlalbb, G::smlalbt,
			G::smlaltb, G::smlaltt};
		push(table[plan.sub]);
		if (plan.sub >= 12)
			push(lo | hi << 8 | n << 16 | m << 24);
		else
			push(d | n << 8 | m << 16 | a << 24);
		return {};
	}
	case Family::div:
		push(plan.sub == 0 ? G::sdiv : G::udiv);
		push(d | n << 8 | m << 16);
		return {};
	case Family::parallel:
		push(kParallel[plan.sub / 6U][plan.sub % 6U]);
		push(d | n << 8 | m << 16);
		return {};
	case Family::sel:
		push(G::sel);
		push(d | n << 8 | m << 16);
		return {};
	default:
		return {};
	}
}

Translator::Result Translator::emit_integer(const Instruction &in,
					    const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto m = static_cast<std::uint64_t>(o.m) * 8U;
	switch (plan.family) {
	case Family::movw: {
		const auto result = emit_mov_constant(in, o.d,
			static_cast<std::uint32_t>(o.imm32));
		if (o.d != 15 && in.cond >= 14 && !in.in_it)
			movw_ = {words_.size(), d};
		return result;
	}
	case Family::movt:
		/* MOVT behind a MOVW of the same register is one move of the
		 * whole constant: the MOVW's immediate takes the top half. */
		if (movw_.end == words_.size() && movw_.rd == d &&
		    in.cond >= 14 && !in.in_it) {
			auto &word = words_[words_.size() - 1U];
			word = (word & ~(std::uintptr_t{0xffff} << 48)) |
			       std::uintptr_t{o.imm16} << 48;
			return {};
		}
		push(G::movt);
		push(d | std::uint64_t{o.imm16} << 48);
		return {};
	case Family::adr: {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto value = o.add ? pc_aligned() + offset :
					   pc_aligned() - offset;
		return emit_mov_constant(in, o.d, value);
	}
	case Family::extend: {
		static constexpr std::array<G, 8> table{
			G::sxtb, G::sxth, G::uxtb, G::uxth, G::sxtba, G::sxtha,
			G::uxtba, G::uxtha};
		push(table[plan.sub]);
		push(d | n << 8 | m << 16 |
		     std::uint64_t{static_cast<unsigned>(o.rotation)} << 24);
		return {};
	}
	case Family::bits: {
		static constexpr std::array<G, 5> table{
			G::rev, G::rev16, G::revsh, G::rbit, G::clz};
		push(table[plan.sub]);
		push(d | m << 16);
		return {};
	}
	case Family::field: {
		const auto lsb = static_cast<unsigned>(o.lsbit);
		if (plan.sub == 0) {
			const unsigned width = o.widthminus1 + 1U;
			push(G::ubfx);
			push(d | n << 8 | std::uint64_t{lsb} << 16 |
			     std::uint64_t{mask_of_width(width)} << 32);
		} else if (plan.sub == 1) {
			const unsigned width = o.widthminus1 + 1U;
			push(G::sbfx);
			push(d | n << 8 |
			     std::uint64_t{32U - lsb - width} << 16 |
			     std::uint64_t{32U - width} << 24);
		} else {
			const unsigned width = o.msbit - o.lsbit + 1U;
			const std::uint64_t mask =
				std::uint64_t{mask_of_width(width)} << lsb;
			if (plan.sub == 2) {
				push(G::bfi);
				push(d | n << 8 | std::uint64_t{lsb} << 16 |
				     (mask & 0xffff'ffffU) << 32);
			} else {
				push(G::bfc);
				push(d | (mask & 0xffff'ffffU) << 32);
			}
		}
		return {};
	}
	case Family::sat: {
		const auto bits = static_cast<unsigned>(o.saturate_to);
		const bool asr = o.shift_t == SRType::ASR;
		std::int64_t maximum;
		std::int64_t minimum;
		if (plan.sub == 0) {
			maximum = (std::int64_t{1} << (bits - 1U)) - 1;
			minimum = -(std::int64_t{1} << (bits - 1U));
			push(asr ? G::ssat_asr : G::ssat_lsl);
		} else {
			maximum = (std::int64_t{1} << bits) - 1;
			minimum = 0;
			push(asr ? G::usat_asr : G::usat_lsl);
		}
		push(d | n << 8 | std::uint64_t{static_cast<unsigned>(
			o.shift_n)} << 16);
		push((static_cast<std::uint64_t>(maximum) & 0xffff'ffffU) |
		     (static_cast<std::uint64_t>(minimum) & 0xffff'ffffU) << 32);
		return {};
	}
	case Family::pkh:
		push(plan.sub == 0 ? G::pkhbt : G::pkhtb);
		push(d | n << 8 | m << 16 |
		     std::uint64_t{static_cast<unsigned>(o.shift_n)} << 24);
		return {};
	default:
		return {};
	}
}

/* The operand word the library gadget and the library slow paths carry
 * (a32_layout.h). */
std::uint64_t Translator::library_operand(const Instruction &in,
					  bool pc_value) const noexcept
{
	return std::uint64_t{in.word} |
	       std::uint64_t{it_.bits()} << UML_GADGET_A32_LIBRARY_IT_SHIFT |
	       std::uint64_t{thumb_} << UML_GADGET_A32_LIBRARY_THUMB_SHIFT |
	       std::uint64_t{pc_value} << UML_GADGET_A32_LIBRARY_PC_VALUE_SHIFT |
	       std::uint64_t{static_cast<std::uint16_t>(in.form)}
		       << UML_GADGET_A32_LIBRARY_FORM_SHIFT;
}

/* An instruction the engine leaves to the semantic library. */
Translator::Result Translator::emit_library(const Instruction &in)
{
	++library_calls_;
	push(G::library);
	push(key_);
	push(library_operand(in));
	return {};
}

/* ------------------------------------------------------- loads, stores */

Translator::Result Translator::emit_single(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::load;
	const unsigned size = plan.sub;
	const bool literal = (plan.aux & 1U) != 0;
	const bool register_form = (plan.aux & 2U) != 0;
	const bool unprivileged = (plan.aux & 4U) != 0;
	const bool to_pc = load && o.t == 15;
	std::uintptr_t rt = static_cast<std::uintptr_t>(o.t) * 8U;
	if (to_pc)
		rt = scratch0;
	else if (!load && o.t == 15)
		rt = source(15, scratch0);

	unsigned mode;
	std::uint64_t word = rt;
	if (literal) {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto address = o.add ? pc_aligned() + offset :
					     pc_aligned() - offset;
		mode = 9;
		word |= std::uint64_t{address} << 32;
	} else if (register_form && !unprivileged && o.n == 15 && o.m != 15 &&
		   !o.wback && o.add && o.shift_t == SRType::LSL &&
		   o.shift_n == 0) {
		/* [PC, Rm] is [Rm, #PC]: the offset form, with no scratch slot
		 * holding the PC value. */
		mode = 0;
		word |= static_cast<std::uint64_t>(o.m) * 8U << 8 |
			std::uint64_t{pc_value()} << 32;
	} else {
		const std::uintptr_t rn = source(o.n, scratch1);
		/* The unprivileged forms post-index in A32 and use the offset
		 * form, with no writeback, in T32. */
		const bool writeback = unprivileged ? o.postindex != 0 : o.wback;
		const bool post = unprivileged ? o.postindex != 0 :
						 (!o.index && o.wback);
		word |= rn << 8;
		if (!register_form) {
			std::uint32_t offset = static_cast<std::uint32_t>(o.imm32);
			if (!o.add)
				offset = 0U - offset;
			mode = !writeback ? 0U : post ? 2U : 1U;
			word |= std::uint64_t{offset} << 32;
		} else {
			std::uintptr_t rm = static_cast<std::uintptr_t>(o.m) * 8U;
			unsigned amount = 0;
			if (o.shift_t == SRType::LSL) {
				amount = static_cast<unsigned>(o.shift_n);
			} else {
				/* A shift the transfer cannot apply is a move
				 * first. */
				(void)emit_dp_values(in, op_mov, false, 1,
						     scratch0 == rt ? scratch1 :
						     scratch0, 0, rm, 0, 0,
						     std::nullopt);
				rm = scratch0 == rt ? scratch1 : scratch0;
			}
			const unsigned base = !writeback ? 3U : post ? 7U : 5U;
			mode = base + (o.add ? 0U : 1U);
			word |= rm << 16 | std::uint64_t{amount} << 32;
		}
	}
	push(load ? kLoad[size][mode] : kStore[size][mode]);
	push(word | position());
	if (to_pc)
		emit_indirect(scratch0, true, o.n == 13);
	return {to_pc, false};
}

Translator::Result Translator::emit_double(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::loadd;
	const bool literal = (plan.aux & 1U) != 0;
	const auto rt = static_cast<std::uint64_t>(o.t) * 8U;
	const auto rt2 = static_cast<std::uint64_t>(o.t2) * 8U;
	std::uint64_t word = rt | rt2 << 8;
	unsigned mode;
	if (literal) {
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		const auto address = o.add ? pc_aligned() + offset :
					     pc_aligned() - offset;
		mode = 9;
		word |= std::uint64_t{address} << 32;
	} else {
		const std::uintptr_t rn = source(o.n, scratch0);
		word |= rn << 16;
		/* The register forms are the A32 encodings with bit 22 clear. */
		const bool by_register = !thumb_ && (in.word & (1U << 22)) == 0;
		if (!by_register) {
			std::uint32_t offset = static_cast<std::uint32_t>(o.imm32);
			if (!o.add)
				offset = 0U - offset;
			mode = !o.wback ? 0U : o.index ? 1U : 2U;
			word |= std::uint64_t{offset} << 32;
		} else {
			const std::uintptr_t rm = source(o.m, scratch1);
			const unsigned base = !o.wback ? 3U : o.index ? 5U : 7U;
			mode = base + (o.add ? 0U : 1U);
			word |= rm << 32;
		}
	}
	push(load ? kLdrd[mode] : kStrd[mode]);
	push(word | position());
	return {};
}

Translator::Result Translator::emit_block_transfer(const Instruction &in,
						   const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool load = plan.family == Family::ldm;
	const int base_register = plan.aux != 0 ? 13 : o.n;
	const bool writeback = plan.aux != 0 || o.wback;
	const unsigned registers = o.registers;
	const int count = std::popcount(registers);
	int start;
	int delta;
	switch (plan.sub) {
	case 0: start = 0; delta = 4 * count; break;
	case 1: start = -4 * count + 4; delta = -4 * count; break;
	case 2: start = -4 * count; delta = -4 * count; break;
	default: start = 4; delta = 4 * count; break;
	}
	const bool has_pc = (registers & 0x8000U) != 0;
	/* The gadgets carry no key, so an STM that stores the PC, which reads
	 * the instruction's address, is the library's. */
	if (!load && has_pc)
		return emit_library(in);
	/* Each register is its CpuState offset over four, which the gadget
	 * scales back in the access: the first six in the top of the word
	 * that holds the base, the rest in one more word. A load of the PC
	 * goes to the scratch slot. */
	auto slot = [](unsigned number) -> std::uint64_t {
		return (number == 15 ? scratch0 : number * 8U) / 4U;
	};
	std::uint64_t word = slot(static_cast<unsigned>(base_register)) |
			     std::uint64_t{writeback} << 5 |
			     std::uint64_t{static_cast<std::uint8_t>(start)} << 8 |
			     std::uint64_t{static_cast<std::uint8_t>(delta)} << 16 |
			     position();
	std::uint64_t rest = 0;
	unsigned index = 0;
	for (unsigned number = 0; number < 16; ++number) {
		if ((registers >> number & 1U) == 0)
			continue;
		if (index < block_word_registers)
			word |= slot(number) << (32U + 5U * index);
		else
			rest |= slot(number)
				<< (5U * (index - block_word_registers));
		++index;
	}
	push(load ? kLdm[count] : kStm[count]);
	push(word);
	if (count > static_cast<int>(block_word_registers))
		push(rest);
	/* A transfer that crosses a page is the library's; a load that writes
	 * PC leaves the value in the scratch slot, as the gadget does. */
	push(library_operand(in, load && has_pc));
	if (load && has_pc) {
		emit_indirect(scratch0, true, base_register == 13);
		return {true, false};
	}
	return {};
}

Translator::Result Translator::emit_exclusive(const Instruction &in,
					      const FormPlan &plan)
{
	const auto &o = in.operands;
	const bool store = (plan.aux & 1U) != 0;
	/* LDAEX and STLEX are the same operations with the host's acquire and
	 * release forms. */
	const bool ordered = (plan.aux & 2U) != 0;
	const unsigned size = plan.sub;
	const auto t = static_cast<std::uint64_t>(o.t) * 8U;
	const auto t2 = static_cast<std::uint64_t>(o.t2) * 8U;
	auto n = static_cast<std::uint64_t>(o.n) * 8U;
	const auto d = static_cast<std::uint64_t>(o.d) * 8U;
	/* The gadgets are the A64 engine's, which carry no PC word: the
	 * instruction's position in the block rides in the operand's top byte
	 * and finish_a32_block turns a fault's position back into the key. */
	const auto position = static_cast<std::uint64_t>(compiled_) << 56;
	/* The T32 forms add a word offset to the base; the gadgets take the
	 * address in a register. */
	if (o.imm32 != 0) {
		(void)emit_dp_values(in, op_add, false, 0, scratch0, n, 0, 0,
				     static_cast<std::uint32_t>(o.imm32),
				     std::nullopt);
		n = scratch0;
	}
	if (!store) {
		if (size == 3) {
			push_external(ordered ?
				uml_arm64_gadget_exclusive_pair_load_acquire_w_rr :
				uml_arm64_gadget_exclusive_pair_load_relaxed_w_rr);
			push(t | t2 << 16 | n << 32 | position);
		} else {
			push_external(ordered ?
				(size == 0 ?
				 uml_arm64_gadget_exclusive_load_acquire_b_r :
				 size == 1 ?
				 uml_arm64_gadget_exclusive_load_acquire_h_r :
				 uml_arm64_gadget_exclusive_load_acquire_w_r) :
				(size == 0 ?
				 uml_arm64_gadget_exclusive_load_relaxed_b_r :
				 size == 1 ?
				 uml_arm64_gadget_exclusive_load_relaxed_h_r :
				 uml_arm64_gadget_exclusive_load_relaxed_w_r));
			push(t | n << 16 | position);
		}
	} else if (size == 3) {
		push_external(ordered ?
			uml_arm64_gadget_exclusive_pair_store_release_w_rrr :
			uml_arm64_gadget_exclusive_pair_store_relaxed_w_rrr);
		push(t | t2 << 12 | d << 24 | n << 36 | position);
	} else {
		push_external(ordered ?
			(size == 0 ?
			 uml_arm64_gadget_exclusive_store_release_b_rr :
			 size == 1 ?
			 uml_arm64_gadget_exclusive_store_release_h_rr :
			 uml_arm64_gadget_exclusive_store_release_w_rr) :
			(size == 0 ?
			 uml_arm64_gadget_exclusive_store_relaxed_b_rr :
			 size == 1 ?
			 uml_arm64_gadget_exclusive_store_relaxed_h_rr :
			 uml_arm64_gadget_exclusive_store_relaxed_w_rr));
		push(t | d << 16 | n << 32 | position);
	}
	return {};
}

/* LDA, LDAB, LDAH, STL, STLB and STLH: the host's load-acquire and
 * store-release, whose alignment rule is the exclusives'. The operands are
 * the register and the register holding the address. */
Translator::Result Translator::emit_ordered(const Instruction &in,
					    const FormPlan &plan)
{
	const auto &o = in.operands;
	const auto t = static_cast<std::uint64_t>(o.t) * 8U;
	const auto n = static_cast<std::uint64_t>(o.n) * 8U;
	if (plan.aux == 0)
		push_external(plan.sub == 0 ?
			uml_arm64_gadget_ordered_load_acquire_b_r :
			plan.sub == 1 ?
			uml_arm64_gadget_ordered_load_acquire_h_r :
			uml_arm64_gadget_ordered_load_acquire_w_r);
	else
		push_external(plan.sub == 0 ?
			uml_arm64_gadget_ordered_store_release_b_r :
			plan.sub == 1 ?
			uml_arm64_gadget_ordered_store_release_h_r :
			uml_arm64_gadget_ordered_store_release_w_r);
	push(t | n << 16 | static_cast<std::uint64_t>(compiled_) << 56);
	return {};
}

/* ------------------------------------------------------------ branches */

Translator::Result Translator::emit_branch(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	switch (plan.family) {
	case Family::branch: {
		const auto target = target_key(
			pc_value() + static_cast<std::uint32_t>(o.imm32), thumb_);
		if (in.cond >= 14) {
			emit_direct_exit(target);
			return {true, false};
		}
		if (linked() && !input_.single) {
			if (compare_.end == words_.size() && !in.in_it) {
				/* The compare just pushed and this branch are
				 * one entry: the AArch64 bank's fused pair
				 * takes the compare's operand word, with the
				 * count of instructions behind the branch in
				 * its top bits, and the link. */
				words_.truncate(words_.size() - 2U);
				push_external((compare_.immediate ?
					compare_immediate_branch_inline :
					compare_register_branch_inline)[in.cond]);
				patches_.push_back({words_.size(), compiled_ + 1,
						    compare_.immediate ? 40U : 48U});
				push(compare_.operand);
				push_link(target);
				return {};
			}
			push_external(conditional_inline[in.cond]);
			push_link(target);
			patches_.push_back({words_.size(), compiled_ + 1, 0});
			push(0);
			return {};
		}
		push_external(conditional_plain[in.cond]);
		push(target);
		push(next_key_);
		return {true, false};
	}
	case Family::bl: {
		const bool to_thumb = o.targetInstrSet == InstrSet::T32;
		const bool exchange = to_thumb != thumb_;
		const auto base = exchange ? pc_aligned() : pc_value();
		const auto target = target_key(
			base + static_cast<std::uint32_t>(o.imm32), to_thumb);
		credit();
		/* The linked form takes LR from the return link's key. */
		if (linked()) {
			push(G::bl_linked);
			push_link(target);
			push_link(next_key_);
		} else {
			push(G::bl);
			push(target);
			push(return_value());
		}
		return {true, false};
	}
	case Family::bx: {
		emit_indirect(source(o.m, scratch0), true, o.m == 14);
		return {true, false};
	}
	case Family::blx: {
		const auto src = source(o.m, scratch0);
		credit();
		if (indirects_ && linked()) {
			push(G::blx_cached);
			push(src);
			push(reinterpret_cast<std::uintptr_t>(indirects_(key_)));
			push_link(next_key_);
		} else {
			push(G::blx);
			push(src);
			push(return_value());
		}
		return {true, false};
	}
	case Family::cbz: {
		const auto target = target_key(
			pc_value() + static_cast<std::uint32_t>(o.imm32), true);
		const auto rn = static_cast<std::uintptr_t>(o.n) * 8U;
		const bool nonzero = plan.sub != 0;
		if (linked() && !input_.single) {
			push(nonzero ? G::cbnz_inline : G::cbz_inline);
			push(rn);
			push_link(target);
			patches_.push_back({words_.size(), compiled_ + 1, 0});
			push(0);
			return {};
		}
		push(nonzero ? G::cbnz_plain : G::cbz_plain);
		push(rn);
		push(target);
		push(next_key_);
		return {true, false};
	}
	case Family::tb: {
		const auto rn = source(o.n, scratch0);
		const auto rm = static_cast<std::uintptr_t>(o.m) * 8U;
		/* The gadget gives back the instructions behind it itself, after
		 * the table load, so that one that faults leaves them charged. */
		static_assert(block_limit <= 256);
		push(plan.sub == 0 ? G::tbb : G::tbh);
		patches_.push_back({words_.size(), compiled_ + 1, 16});
		push(rn | rm << 8 | position() |
		     std::uint64_t{pc_value()} << 32);
		push(indirects_ ? reinterpret_cast<std::uintptr_t>(indirects_(key_))
				: reinterpret_cast<std::uintptr_t>(&dummy_site()));
		return {true, false};
	}
	default:
		return {};
	}
}

/* --------------------------------------------------------------- system */

Translator::Result Translator::emit_system(const Instruction &in,
					   const FormPlan &plan)
{
	const auto &o = in.operands;
	switch (plan.family) {
	case Family::svc:
		credit();
		push(G::svc);
		push(next_key_);
		return {true, false};
	case Family::bkpt:
		credit();
		push(G::bkpt);
		push(key_);
		return {true, false};
	case Family::hint:
		if (plan.aux != 0)
			push_external(uml_arm64_gadget_yield);
		return {};
	case Family::barrier:
		if (plan.sub == 2) {
			push(G::isb);
			synchronize_ = true;
		} else if (plan.sub == 0)
			push_external(data_barrier[in.word & 15U]);
		else
			push_external(synchronization_barrier[in.word & 15U]);
		return {};
	case Family::clrex:
		push_external(uml_arm64_gadget_clear_exclusive);
		return {};
	case Family::mrs:
		push(G::mrs);
		push(static_cast<std::uint64_t>(o.d) * 8U);
		return {};
	case Family::msr: {
		const unsigned mask = thumb_ ? in.word >> 10 & 3U :
					       in.word >> 18 & 3U;
		std::uintptr_t rn;
		if (in.form == Form::MSR_i_A1_AS) {
			set_scratch(scratch0, static_cast<std::uint32_t>(o.imm32));
			rn = scratch0;
		} else {
			rn = static_cast<std::uintptr_t>(o.n) * 8U;
		}
		if (mask == 0)
			return {};
		push(G::msr);
		push(rn | std::uint64_t{(mask >> 1) & 1U} << 8 |
		     std::uint64_t{mask & 1U} << 9);
		return {false, (mask & 2U) != 0};
	}
	case Family::cp15: {
		const auto t = static_cast<std::uint64_t>(o.t) * 8U;
		switch (in.sysreg) {
		case SysReg::tpidruro:
			push(G::read_tpidruro);
			push(t);
			return {};
		case SysReg::tpidrurw_read:
			push(G::read_tpidrurw);
			push(t);
			return {};
		case SysReg::tpidrurw_write:
			push(G::write_tpidrurw);
			push(t);
			return {};
		case SysReg::cntfrq:
			push(G::read_cntfrq);
			push(t);
			return {};
		case SysReg::cntvct:
			push(G::read_cntvct);
			push(t | static_cast<std::uint64_t>(o.t2) * 8U << 8);
			return {};
		case SysReg::cp15_dsb:
			push(G::cp15_dsb);
			push(key_);
			return {};
		case SysReg::cp15_dmb:
			push(G::cp15_dmb);
			push(key_);
			return {};
		case SysReg::cp15_isb:
			push(G::cp15_isb);
			push(key_);
			synchronize_ = true;
			return {};
		case SysReg::none:
			break;
		}
		return {};
	}
	default:
		return {};
	}
}

Translator::Result Translator::emit(const Instruction &in,
				    const FormPlan &plan)
{
	switch (plan.family) {
	case Family::dp:
		return emit_dp(in, plan);
	case Family::mul:
	case Family::mull:
	case Family::mulh:
	case Family::mulxy:
	case Family::div:
	case Family::parallel:
	case Family::sel:
		return emit_multiply(in, plan);
	case Family::movw:
	case Family::movt:
	case Family::adr:
	case Family::extend:
	case Family::bits:
	case Family::field:
	case Family::sat:
	case Family::pkh:
		return emit_integer(in, plan);
	case Family::library:
		return emit_library(in);
	case Family::load:
	case Family::store:
		return emit_single(in, plan);
	case Family::loadd:
	case Family::stored:
		return emit_double(in, plan);
	case Family::ldm:
	case Family::stm:
		return emit_block_transfer(in, plan);
	case Family::exclusive:
		return emit_exclusive(in, plan);
	case Family::ordered:
		return emit_ordered(in, plan);
	case Family::branch:
	case Family::bl:
	case Family::bx:
	case Family::blx:
	case Family::cbz:
	case Family::tb:
		return emit_branch(in, plan);
	case Family::it:
		return {};
	case Family::svc:
	case Family::bkpt:
	case Family::hint:
	case Family::barrier:
	case Family::clrex:
	case Family::mrs:
	case Family::msr:
	case Family::cp15:
		return emit_system(in, plan);
	case Family::vfp3:
	case Family::vfpmac:
	case Family::vfp2:
	case Family::vfpcmp:
	case Family::vfpimm:
	case Family::vfpcvt:
	case Family::vfpcore:
	case Family::vfpsys:
	case Family::vfpmem:
	case Family::vfpblock:
		return emit_vfp(in, plan);
	case Family::neon:
		return emit_neon(in, plan);
	case Family::neonmem:
		return emit_structure(in, plan);
	case Family::crc:
	case Family::vfpv8:
	case Family::vfpcvt8:
	case Family::vfphalf:
		return emit_armv8(in, plan);
	case Family::udf:
	case Family::none:
		break;
	}
	return {};
}

/* ----------------------------------------------------------------- run */

BuildOutput Translator::run()
{
	const auto *bytes = input_.code.data();
	const auto half = [&](std::size_t unit) {
		std::uint16_t value;
		std::memcpy(&value, bytes + unit * 2U, sizeof(value));
		return value;
	};
	const auto word_at = [&](std::size_t unit) {
		std::uint32_t value;
		std::memcpy(&value, bytes + unit * 4U, sizeof(value));
		return value;
	};

	while (!ended_ && compiled_ < limit_) {
		if (!input_.single && compiled_ >= soft_limit_ && !it_.active())
			break;
		key_ = input_.key + 4U * unit_;
		if (unit_ >= available_)
			break;
		std::size_t length = 1;
		std::uint32_t raw;
		Instruction in;
		if (thumb_) {
			const std::uint16_t hw1 = half(unit_);
			if (thumb_length(hw1) == 4) {
				if (unit_ + 1 >= available_)
					break;
				length = 2;
				raw = std::uint32_t{hw1} << 16 | half(unit_ + 1);
			} else {
				raw = std::uint32_t{hw1} << 16;
			}
			in = decode_thumb(raw, it_, profile_);
		} else {
			raw = word_at(unit_);
			in = decode_a32(raw, profile_);
		}
		next_key_ = key_ + 4U * length;
		const FormPlan plan = kFormPlans[static_cast<std::size_t>(in.form)];
		/* A coprocessor access the profile does not accept, and a
		 * coprocessor instruction of an extension the profile lacks, are
		 * undefined only when their condition passes (the pseudocode
		 * tests ConditionPassed first). Every other undefined word is
		 * undefined whatever the condition: the profile leaves it out at
		 * decode time, it is unallocated, or it is UNPREDICTABLE, and
		 * so is a coprocessor word the full profile finds UNPREDICTABLE
		 * (an encoding that is only valid under AL). The decoder
		 * reports an undefined word's condition as 14, so a conditional
		 * one takes the word's own field. */
		if (in.status == Status::undefined) {
			unsigned cond = 14;
			const Profile full;
			if (thumb_) {
				if (in.in_it && (in.reason == Reason::profile ||
						 in.reason == Reason::feature)) {
					const auto whole = decode_thumb(raw, it_, full);
					if (whole.status != Status::undefined ||
					    whole.reason != Reason::unpredictable)
						cond = in.cond;
				}
			} else {
				const bool coprocessor = (raw >> 25 & 7U) == 6U ||
							 (raw >> 24 & 15U) == 14U;
				if (coprocessor && (raw >> 28) < 14U &&
				    (in.reason == Reason::profile ||
				     in.reason == Reason::feature ||
				     in.reason == Reason::unallocated)) {
					const auto whole = decode_a32(raw, full);
					if (whole.status != Status::undefined ||
					    whole.reason != Reason::unpredictable)
						cond = raw >> 28;
				}
			}
			if (cond < 14) {
				begin_instruction(cond);
				push_external(uml_arm64_gadget_illegal);
				push(key_);
				it_ = it_.advance();
				unit_ += length;
				units_ = unit_;
				++compiled_;
				if (input_.single)
					break;
				continue;
			}
		}
		if (in.status == Status::undefined ||
		    plan.family == Family::none || plan.family == Family::udf) {
			units_ = unit_ + length;
			finish_stop(true, raw);
			break;
		}
		if (in.status == Status::ok) {
			/* A conditional branch tests its own condition, and
			 * conditional data processing that sets no flags and a
			 * conditional VMOV of VFP registers are selects where
			 * emit_dp_values or emit_vfp has one for the shape;
			 * every other conditional instruction is behind a
			 * skip. */
			if (plan.family == Family::branch ||
			    plan.family == Family::cbz || plan.family == Family::it) {
				close_run();
			} else if (in.cond < 14 &&
				   ((!in.operands.setflags &&
				     in.operands.d != 15 &&
				     (plan.family == Family::dp ||
				      plan.family == Family::movw)) ||
				    (plan.family == Family::vfp2 &&
				     plan.sub == kVfp2Vmov))) {
				close_run();
				select_ = in.cond;
			} else {
				begin_instruction(in.cond);
			}
			/* The vectors and FPSCR load lazily, once, for a block
			 * marked as touching them (uses_fp_): every form that
			 * does, by the decoder table's fp_state trait, whichever
			 * path lowers it. */
			if ((in.info().traits & aarch32::trait::fp_state) != 0U)
				uses_fp_ = true;
			const std::size_t address_add = address_add_.start;
			const std::size_t library_calls = library_calls_;
			const Result result = emit(in, plan);
			/* An ADD this instruction did not push stays absorbable
			 * only behind Advanced SIMD data processing that reads no
			 * core register, cannot leave the block and is not
			 * conditional, so that moving the ADD past it changes no
			 * state any exit or fault can see. */
			if (address_add_.start == address_add &&
			    !(plan.family == Family::neon && in.cond == 14 &&
			      !in.in_it && library_calls_ == library_calls &&
			      kNeonGroups[plan.sub].shape != NeonShape::dcore))
				address_add_.start = ~std::size_t{};
			if (result.flags)
				flags_written();
			if (result.terminal && !run_active_)
				ended_ = true;
		}
		it_ = in.next_it;
		unit_ += length;
		units_ = unit_;
		++compiled_;
		if (input_.single || synchronize_)
			break;
	}

	const bool stopped = !complete_;
	if (!stopped && !ended_) {
		units_ = unit_;
		emit_fallthrough();
	} else {
		close_run();
	}
	for (const auto &patch : patches_)
		words_[patch.index] |= std::uintptr_t{
			compiled_ > patch.through ? compiled_ - patch.through : 0}
			<< patch.shift;

	BuildOutput output{
		BuildResult{Program{input_.key, compiled_, std::move(words_),
				    uses_fp_},
			    complete_, stop_address_, stop_instruction_},
		units_, it_};
	return output;
}

} // namespace detail

BuildOutput build(const BuildInput &input,
		  const Builder::LinkResolver &resolve_link,
		  std::pmr::memory_resource *storage,
		  const Builder::IndirectLinkResolver &resolve_indirect_link,
		  const BuildOptions &, const Profile &profile,
		  std::size_t block_instructions)
{
	return detail::Translator{input, resolve_link, storage, resolve_indirect_link,
			  profile, block_instructions}.run();
}

aarch32::Profile decode_profile(const BuildOptions &options) noexcept
{
	/* The shared profile (a32_decode_profile): a form of a shown extension
	 * that has no gadget is the semantic library's. */
	return a32_decode_profile(options.host_capabilities);
}

std::optional<std::size_t> thumb_position(
	std::span<const std::byte> halfwords, std::size_t unit) noexcept
{
	std::size_t position = 0;
	std::size_t at = 0;
	const auto count = halfwords.size() / 2U;
	while (at < count) {
		if (at == unit)
			return position;
		if (at > unit)
			return std::nullopt;
		std::uint16_t hw1;
		std::memcpy(&hw1, halfwords.data() + at * 2U, sizeof(hw1));
		at += thumb_length(hw1) / 2U;
		++position;
	}
	return at == unit ? std::optional{position} : std::nullopt;
}

std::optional<std::size_t> thumb_unit(
	std::span<const std::byte> halfwords, std::size_t position) noexcept
{
	std::size_t at = 0;
	const auto count = halfwords.size() / 2U;
	for (std::size_t seen = 0; at < count; ++seen) {
		if (seen == position)
			return at;
		std::uint16_t hw1;
		std::memcpy(&hw1, halfwords.data() + at * 2U, sizeof(hw1));
		at += thumb_length(hw1) / 2U;
	}
	return std::nullopt;
}

bool routes_to_library(aarch32::Form form) noexcept
{
	return kFormPlans[static_cast<std::size_t>(form)].family ==
	       Family::library;
}

bool may_reach_library(aarch32::Form form) noexcept
{
	const auto family =
		kFormPlans[static_cast<std::size_t>(form)].family;
	return family == Family::library || family == Family::ldm ||
	       family == Family::stm || family == Family::vfpblock ||
	       family == Family::neonmem;
}

std::uint8_t thumb_itstate(std::span<const std::byte> halfwords,
			   std::size_t unit, const Profile &profile) noexcept
{
	ItState state;
	std::size_t at = 0;
	const auto count = halfwords.size() / 2U;
	while (at < count && at < unit) {
		std::uint16_t hw1;
		std::uint16_t hw2 = 0;
		std::memcpy(&hw1, halfwords.data() + at * 2U, sizeof(hw1));
		const auto length = thumb_length(hw1) / 2U;
		if (length == 2) {
			if (at + 1 >= count)
				break;
			std::memcpy(&hw2, halfwords.data() + (at + 1) * 2U,
				    sizeof(hw2));
		}
		const auto decoded = decode_thumb(
			std::uint32_t{hw1} << 16 | hw2, state, profile);
		state = decoded.next_it;
		at += length;
	}
	return state.bits();
}

} // namespace uml::cpu::arm64::gadgets::a32
