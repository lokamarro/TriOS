// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The AArch32 translator the gadget engine's builder (program_builder_a32.cpp)
 * is made of. The class is declared here so that the families whose lowering
 * is long -- the integer core, VFP, Advanced SIMD -- can be defined in
 * translation units of their own; nothing outside the builder's files
 * includes this header. */
#include "a32_builder.hpp"
#include "a32_forms.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/gadgets/a32_layout.h"
#include "cpu/arm64/layout/assembly_offsets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets::a32::detail {

using namespace aarch32;

/* The part's scratch slots (a32_layout.h), where PC read as an operand, a
 * destination PC and a T32 exclusive transfer's address live. */
constexpr std::uintptr_t scratch0 = UML_GADGET_A32_SCRATCH0_SLOT * 8;
constexpr std::uintptr_t scratch1 = UML_GADGET_A32_SCRATCH1_SLOT * 8;
/* The registers of a block transfer that its first operand word holds
 * (engine/generate_a32.py). */
constexpr unsigned block_word_registers = 6;

/* The CpuState offset of core register `number`, as an operand word packs
 * it. */
[[nodiscard]] inline std::uint64_t core(int number) noexcept
{
	return static_cast<std::uint64_t>(number) * 8U;
}

/* Whether condition `cond` passes, for each NZCV value n (N in bit 3, V in
 * bit 0): bit n of the result. The truth table a select gadget tests. */
[[nodiscard]] inline constexpr std::uint64_t condition_table(unsigned cond) noexcept
{
	std::uint64_t table = 0;
	for (unsigned nzcv = 0; nzcv < 16; ++nzcv) {
		const bool n = (nzcv & 8U) != 0, z = (nzcv & 4U) != 0;
		const bool c = (nzcv & 2U) != 0, v = (nzcv & 1U) != 0;
		bool pass = true;
		switch (cond >> 1) {
		case 0: pass = z; break;
		case 1: pass = c; break;
		case 2: pass = n; break;
		case 3: pass = v; break;
		case 4: pass = c && !z; break;
		case 5: pass = n == v; break;
		case 6: pass = !z && n == v; break;
		default: break;
		}
		if ((cond & 1U) != 0 && cond != 15)
			pass = !pass;
		if (pass)
			table |= std::uint64_t{1} << nzcv;
	}
	return table;
}
static_assert(condition_table(0) == 0xf0f0U);  /* EQ: Z */
static_assert(condition_table(11) == 0x55aaU); /* LT: N != V */
static_assert(condition_table(14) == 0xffffU);

/* Where D0-D31 and S0-S31 are: they overlay V0-V15 (D(2n) is the low half
 * of V(n)), so a VFP or Advanced SIMD register is a byte offset. */
constexpr std::uint64_t vector_base = UML_GADGET_CPU_V0;
[[nodiscard]] constexpr std::uint64_t s_register(int number) noexcept
{
	return vector_base + 4U * static_cast<std::uint64_t>(number);
}
[[nodiscard]] constexpr std::uint64_t d_register(int number) noexcept
{
	return vector_base + 8U * static_cast<std::uint64_t>(number);
}

template <typename Gadget>
[[nodiscard]] std::uintptr_t address_of(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

class Translator final {
public:
	Translator(const BuildInput &input,
		   const Builder::LinkResolver &links,
		   std::pmr::memory_resource *storage,
		   const Builder::IndirectLinkResolver &indirects,
		   const Profile &profile, std::size_t block_instructions)
		: input_{input}, links_{links}, indirects_{indirects},
		  profile_{profile}, words_{storage}, it_{input.it},
		  thumb_{key_isa(input.key) == GuestIsa::t32},
		  unit_bytes_{thumb_ ? 2U : 4U},
		  limit_{input.single ? 1U :
				       std::min(block_instructions, block_limit)},
		  /* A T32 block stops early enough for an IT block it starts
		   * to complete inside it; A32 has none. */
		  soft_limit_{input.single ? 1U : thumb_ ? limit_ - 5U : limit_},
		  available_{input.code.size() / unit_bytes_}
	{}

	[[nodiscard]] BuildOutput run();

private:
	struct Result final {
		bool terminal{};
		bool flags{};
	};

	/* ---------------------------------------------------------- output */
	void push(std::uintptr_t word) { words_.push_back(word); }
	void push(G which) { words_.push_back(gadget(which)); }
	template <typename Gadget>
	void push_external(Gadget which)
	{
		words_.push_back(address_of(which));
	}
	void push_link(std::uint64_t key)
	{
		push(reinterpret_cast<std::uintptr_t>(links_(key)));
	}
	[[nodiscard]] bool linked() const noexcept { return !!links_; }

	/* ------------------------------------------------------- positions */
	[[nodiscard]] std::uint32_t pc_value() const noexcept
	{
		return key_pc(key_) + (thumb_ ? 4U : 8U);
	}
	[[nodiscard]] std::uint32_t pc_aligned() const noexcept
	{
		return pc_value() & ~3U;
	}
	[[nodiscard]] static std::uint64_t target_key(std::uint32_t address,
						      bool to_thumb) noexcept
	{
		return to_thumb ? t32_key(address & ~1U) : a32_key(address);
	}
	/* The value LR gets for a call returning to the next instruction. */
	[[nodiscard]] std::uint32_t return_value() const noexcept
	{
		return key_pc(next_key_) | (thumb_ ? 1U : 0U);
	}

	/* ------------------------------------------------ conditional runs */
	void close_run();
	void begin_instruction(unsigned cond);
	void flags_written() noexcept { run_dirty_ = true; }
	/* A terminal that does not end the block gives back the instructions
	 * translated after it, which the block's precharge counted. */
	void credit();

	/* --------------------------------------------------- the registers */
	[[nodiscard]] std::uintptr_t source(int number, std::uintptr_t slot);
	void set_scratch(std::uintptr_t slot, std::uint32_t value);

	/* -------------------------------------------------------- families */
	Result emit(const Instruction &in, const FormPlan &plan);
	Result emit_dp(const Instruction &in, const FormPlan &plan);
	Result emit_dp_values(const Instruction &in, unsigned op, bool s,
			      unsigned shape_class, std::uintptr_t rd,
			      std::uintptr_t rn, std::uintptr_t rm,
			      std::uintptr_t rs, std::uint32_t immediate,
			      std::optional<unsigned> carry,
			      bool in_place = true);
	Result emit_mov_constant(const Instruction &in, int d,
				 std::uint32_t value);
	Result emit_multiply(const Instruction &in, const FormPlan &plan);
	Result emit_integer(const Instruction &in, const FormPlan &plan);
	Result emit_single(const Instruction &in, const FormPlan &plan);
	Result emit_double(const Instruction &in, const FormPlan &plan);
	Result emit_block_transfer(const Instruction &in, const FormPlan &plan);
	Result emit_exclusive(const Instruction &in, const FormPlan &plan);
	Result emit_ordered(const Instruction &in, const FormPlan &plan);
	Result emit_branch(const Instruction &in, const FormPlan &plan);
	Result emit_system(const Instruction &in, const FormPlan &plan);
	Result emit_library(const Instruction &in);
	[[nodiscard]] std::uint64_t library_operand(const Instruction &in,
						    bool pc_value = false) const noexcept;
	/* VFP: program_builder_a32_vfp.cpp. */
	Result emit_vfp(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_convert(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_core(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_memory(const Instruction &in, const FormPlan &plan);
	/* Advanced SIMD: program_builder_a32_neon.cpp. */
	Result emit_neon(const Instruction &in, const FormPlan &plan);
	/* VLDn and VSTn. */
	Result emit_structure(const Instruction &in, const FormPlan &plan);
	/* The Armv8 additions and optional extensions that are neither VFP nor
	 * Advanced SIMD: program_builder_a32_armv8.cpp. */
	Result emit_armv8(const Instruction &in, const FormPlan &plan);
	/* The operand bits that name the current instruction to a memory
	 * gadget's fault (a32_layout.h). block_limit keeps every position of a
	 * block within the eight bits. */
	[[nodiscard]] std::uint64_t position() const noexcept
	{
		static_assert(block_limit <= 256);
		return static_cast<std::uint64_t>(compiled_)
		       << UML_GADGET_A32_POSITION_SHIFT;
	}
	/* The illegal-instruction exit at the current instruction. */
	void emit_illegal();
	void emit_indirect(std::uintptr_t source, bool interwork, bool returns);
	void emit_direct_exit(std::uint64_t target);
	void emit_fallthrough();
	void finish_stop(bool instruction_read, std::uint32_t word);

	const BuildInput &input_;
	const Builder::LinkResolver &links_;
	const Builder::IndirectLinkResolver &indirects_;
	const Profile &profile_;
	ProgramWords words_;
	ItState it_;
	bool thumb_;
	std::size_t unit_bytes_;
	std::size_t limit_;
	std::size_t soft_limit_;
	std::size_t available_;

	std::size_t unit_{};
	std::size_t compiled_{};
	std::uint64_t key_{};
	std::uint64_t next_key_{};
	bool ended_{};
	/* The block ends after an ISB, at a fall-through that checks whether
	 * the address space's code changed since it was validated. */
	bool synchronize_{};
	bool complete_{true};
	std::uint64_t stop_address_{};
	std::uint32_t stop_instruction_{};
	std::size_t units_{};

	bool run_active_{};
	unsigned run_cond_{14};
	std::size_t run_operand_{};
	bool run_dirty_{};
	/* Operands the end of the block patches: the word, the number of
	 * instructions translated through the one that holds it, and the bit
	 * the count is placed at in that word. */
	struct Patch final {
		std::size_t index;
		std::size_t through;
		unsigned shift;
	};
	std::vector<Patch> patches_;
	/* The compare the last gadget pushed is, with the operands a fused
	 * compare-and-branch takes, while nothing has been pushed since:
	 * `end` is the length of the stream just after it. */
	struct Compare final {
		std::size_t end{~std::size_t{}};
		bool immediate{};
		std::uint64_t operand{};
	};
	Compare compare_;
	/* The MOVW the last gadget pushed is, while nothing has been pushed
	 * since: `end` is the length of the stream just after it, and `rd` the
	 * register it writes. */
	struct MoveWide final {
		std::size_t end{~std::size_t{}};
		std::uintptr_t rd{};
	};
	MoveWide movw_;
	/* The condition of the instruction being translated when it is a
	 * conditional data-processing instruction that sets no flags, which
	 * emit_dp_values lowers as a select rather than behind a skip; 14
	 * otherwise. */
	unsigned select_{14};
	/* The ADD or SUB of an immediate pushed last, while every instruction
	 * translated since is Advanced SIMD data processing that reads no core
	 * register: a VLD1 or VST1 copy without writeback based on its Rd
	 * absorbs it (emit_structure), taking its words out of the stream.
	 * `start` is ~0 when there is none. */
	struct AddressAdd final {
		std::size_t start{~std::size_t{}};
		std::size_t end{};
		std::uintptr_t rd{};
		std::uintptr_t rn{};
		std::uint32_t immediate{};
	};
	AddressAdd address_add_;
	/* Library exits pushed so far. */
	std::size_t library_calls_{};
	/* The unconditional VCMP or VCMPE pushed last, while nothing has been
	 * pushed since: `end` is the length of the stream just after it, ~0
	 * when there is none, and `operation` and `precision` its kVfpCmpApsr
	 * row and column. A VMRS APSR_nzcv right after it turns its gadget
	 * into the fused one. */
	struct VfpCompare final {
		std::size_t end{~std::size_t{}};
		unsigned operation{};
		unsigned precision{};
	};
	VfpCompare vfp_compare_;
	/* The block holds a form that reads or writes the vector registers or
	 * FPSCR (the decoder table's fp_state trait, set in run()), and so is
	 * entered through the FP state link: CpuState loads the task's vectors
	 * lazily, and only for such a block. */
	bool uses_fp_{};
};

} // namespace uml::cpu::arm64::gadgets::a32::detail
