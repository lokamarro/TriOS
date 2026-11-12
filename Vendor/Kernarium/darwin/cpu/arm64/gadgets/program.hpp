// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../aarch32/guest_key.hpp"
#include "../cpu_state.hpp"
#include "../layout/assembly_offsets.h"
#include "../../engine.hpp"
#include "../fp_environment.hpp"
#include "tlb.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>

namespace uml::cpu::arm64::gadgets {

/* A translated stream's words: either a buffer of its own, taken from the
 * given resource and grown from the builder's 34-word reservation, or words
 * it borrows. */
class ProgramWords final {
public:
	/* No words are kept inline. A measured browser-like guest translates a
	 * mean of 37.5 words per block since conditional fall-throughs are
	 * inlined, so a sixteen-word inline array was unused by the 63.5% that
	 * spill while still costing its bytes in every stored program. */
	static constexpr std::size_t inline_capacity = 0;

	/* Words the program refers to but does not own.
	 *
	 * The cache stores a translation as one exact-sized allocation whose
	 * tail holds the stream, so the program describes those words rather
	 * than keeping a second copy of them. */
	struct Borrowed final {
		std::uintptr_t *data;
		std::size_t size;
	};

	explicit ProgramWords(std::pmr::memory_resource *resource) noexcept
		: resource_{resource}
	{}

	explicit ProgramWords(Borrowed borrowed) noexcept
		: resource_{nullptr}, size_{borrowed.size},
		  capacity_{borrowed.size}, heap_{borrowed.data},
		  borrowed_{true}
	{}

	ProgramWords(const ProgramWords &other)
		: resource_{std::pmr::get_default_resource()}
	{
		reserve(other.size_);
		std::copy_n(other.data(), other.size_, data());
		size_ = other.size_;
	}

	ProgramWords &operator=(const ProgramWords &other)
	{
		if (this == &other)
			return *this;
		reserve(other.size_);
		std::copy_n(other.data(), other.size_, data());
		size_ = other.size_;
		return *this;
	}

	ProgramWords(ProgramWords &&other) noexcept
		: resource_{other.resource_}, size_{other.size_},
		  capacity_{other.capacity_}, heap_{other.heap_},
		  borrowed_{other.borrowed_}
	{
		other.size_ = 0;
		other.capacity_ = inline_capacity;
		other.heap_ = nullptr;
		other.borrowed_ = false;
	}

	ProgramWords &operator=(ProgramWords &&other) noexcept
	{
		if (this == &other)
			return *this;
		release();
		resource_ = other.resource_;
		size_ = other.size_;
		capacity_ = other.capacity_;
		heap_ = other.heap_;
		borrowed_ = other.borrowed_;
		other.size_ = 0;
		other.capacity_ = inline_capacity;
		other.heap_ = nullptr;
		other.borrowed_ = false;
		return *this;
	}

	~ProgramWords() { release(); }

	void push_back(std::uintptr_t word)
	{
		if (size_ == capacity_)
			reserve(capacity_ == inline_capacity ? 34 : capacity_ * 2);
		data()[size_++] = word;
	}

	void reserve(std::size_t requested)
	{
		/* Borrowed words are exactly the stream the cache allocated, and
		 * nothing appends to or assigns over a stored program. Reaching
		 * here with them is a bug either way, so let it fault on the
		 * absent resource rather than quietly rewrite a live stream that
		 * happened to be long enough. */
		if (!borrowed_ && requested <= capacity_)
			return;
		assert(!borrowed_);
		auto *replacement = static_cast<std::uintptr_t *>(
			resource_->allocate(requested * sizeof(std::uintptr_t),
				alignof(std::uintptr_t)));
		std::copy_n(data(), size_, replacement);
		if (heap_ != nullptr)
			resource_->deallocate(heap_,
				capacity_ * sizeof(std::uintptr_t),
				alignof(std::uintptr_t));
		heap_ = replacement;
		capacity_ = requested;
	}

	[[nodiscard]] std::uintptr_t *data() noexcept
	{
		return heap_ != nullptr ? heap_ : inline_.data();
	}
	[[nodiscard]] const std::uintptr_t *data() const noexcept
	{
		return heap_ != nullptr ? heap_ : inline_.data();
	}
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
	/* Drop the words from `size` on, which a decode emitted and the builder
	 * then rejected. */
	void truncate(std::size_t size) noexcept
	{
		if (size < size_)
			size_ = size;
	}
	[[nodiscard]] std::uintptr_t &operator[](std::size_t index) noexcept
	{
		return data()[index];
	}
	[[nodiscard]] const std::uintptr_t &operator[](
		std::size_t index) const noexcept
	{
		return data()[index];
	}

private:
	void release() noexcept
	{
		if (heap_ != nullptr && !borrowed_)
			resource_->deallocate(heap_,
				capacity_ * sizeof(std::uintptr_t),
				alignof(std::uintptr_t));
	}

	std::pmr::memory_resource *resource_;
	std::size_t size_{};
	std::size_t capacity_{inline_capacity};
	std::uintptr_t *heap_{};
	bool borrowed_{};
	std::array<std::uintptr_t, inline_capacity> inline_;
};

class Program final {
public:
	Program(std::uint64_t guest_address, std::size_t instruction_count,
		ProgramWords words,
		bool accesses_fp_state) noexcept;

	/* Where the block starts; its instructions are contiguous from here. */
	[[nodiscard]] std::uint64_t guest_address() const noexcept
	{
		return guest_address_;
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return instruction_count_;
	}
	[[nodiscard]] std::span<const std::uintptr_t> words() const noexcept
	{
		return {words_.data(), words_.size()};
	}
	[[nodiscard]] bool accesses_fp_state() const noexcept
	{
		return accesses_fp_state_;
	}

private:
	std::uint64_t guest_address_{};
	std::size_t instruction_count_{};
	ProgramWords words_;
	bool accesses_fp_state_{};
};

/* Where a guest PC sits in a program, counted in retired instructions from its
 * entry. Empty when the PC is not one this program translated, which is how a
 * caller distinguishes "not ours" from position zero. */
[[nodiscard]] inline std::optional<std::size_t> program_position(
	const Program &program, std::uint64_t pc) noexcept
{
	const auto span = std::uint64_t{program.instruction_count()} *
		sizeof(std::uint32_t);
	if (pc < program.guest_address() || pc - program.guest_address() >= span)
		return std::nullopt;
	const auto offset = pc - program.guest_address();
	if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
		return std::nullopt;
	return static_cast<std::size_t>(offset / sizeof(std::uint32_t));
}

/* Whether `stream` is one of this program's own words, which is how the
 * boundary tells which block an exit left from. */
[[nodiscard]] inline bool program_contains_stream(
	const Program &program, const void *stream) noexcept
{
	if (stream == nullptr || program.words().empty())
		return false;
	const auto marker = reinterpret_cast<std::uintptr_t>(stream);
	const auto begin = reinterpret_cast<std::uintptr_t>(
		program.words().data());
	const auto end =
		begin + program.words().size() * sizeof(std::uintptr_t);
	return marker >= begin && marker < end;
}

/* A memory transfer carries no PC word. Its operand holds the instruction's
 * position in the block in bits 63:56 instead, so the block it names holds at
 * most this many instructions. */
inline constexpr std::size_t fault_position_instructions = 256;

/* A fault in such a transfer reports its position shifted left by two with
 * bit 0 set, which no instruction address has; this puts the PC of the
 * instruction at that position in its place. `program` must be the one the
 * fault left from. A T32 block's instructions have two lengths, so its
 * positions are resolved from its halfwords by the AArch32 boundary. */
inline void resolve_fault_position(
	const Program &program, std::uint64_t &pc) noexcept
{
	if ((pc & (sizeof(std::uint32_t) - 1U)) != 1U ||
	    aarch32::key_is_thumb(program.guest_address()))
		return;
	const auto position = pc >> 2;
	if (position < program.instruction_count())
		pc = program.guest_address() + position * sizeof(std::uint32_t);
}

struct BuildResult final {
	Program program;
	bool complete{};
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
};

/* A conditional branch whose fall-through was translated inline behind it.
 *
 * Entering a block precharges its whole length, so a taken branch leaving from
 * the middle has to give back the instructions after it. That count is not
 * known until the block ends, so each site is recorded here with the number of
 * instructions translated before it, and filled in once the length is final.
 */
struct InlineSuffixes final {
	/* One per conditional branch in a block, which cannot exceed the number
	 * of instructions the fetch window holds. */
	static constexpr std::size_t capacity = 64;

	std::array<std::uint32_t, capacity> word{};
	std::array<std::uint32_t, capacity> compiled{};
	std::array<std::uint8_t, capacity> shift{};
	std::uint32_t count{};

	[[nodiscard]] bool full() const noexcept { return count == capacity; }

	void add(std::size_t at, std::size_t translated,
		 std::uint8_t suffix_shift = 0) noexcept
	{
		word[count] = static_cast<std::uint32_t>(at);
		compiled[count] = static_cast<std::uint32_t>(translated);
		shift[count] = suffix_shift;
		++count;
	}
};

struct BuildOptions final {
	/* Select adjacent write-through forwarding for this program. */
	bool scalar_forwarding{true};
	bool vector_forwarding{true};
	/* Test and portability validation can force optional checksum operations
	 * through their precompiled software gadgets. Production leaves this
	 * false and selects from detected host features.
	 */
	bool force_software_checksum{};
	/* Exercise portable flag-manipulation bodies even on a capable host.
	 * Production selects native fixed-form gadgets at translation time.
	 */
	bool force_software_flags{};
	/* Exercise portable SIMD logical/rotation bodies while leaving the JIT
	 * portion of a shared conformance case unchanged.
	 */
	bool force_software_sha3{};
	/* Exercise portable fixed-transform bodies on capable hosts. Production
	 * resolves each optional host family once while translating.
	 */
	bool force_software_fixed_transforms{};
	/* Exercise the portable 128-bit block-transform bodies on a capable
	 * host. Production selects the native path once at translation time.
	 */
	bool force_software_aes{};
	/* Exercise the portable 64-bit polynomial-multiply gadget even on a host
	 * with the optional native instruction. Byte forms remain native because
	 * they are part of baseline AdvSIMD.
	 */
	bool force_software_pmull{};
	/* Exercise the portable integer dot-product bodies on capable hosts.
	 * Production selects native bodies independently for the ordinary and
	 * mixed-sign families from detected host features.
	 */
	bool force_software_dotprod{};
	bool force_software_i8mm{};
	/* The guest SVCR mode the program runs in (svcr_sm, svcr_za), which
	 * decides what each SME and floating-point word may compile to. */
	std::uint8_t svcr{};
	::uml::cpu::HostCapabilities host_capabilities{
		::uml::cpu::host_capabilities()};

	/* Two executors build the same stream for the same guest code exactly
	 * when their options compare equal, so only then may they share it. */
	[[nodiscard]] bool operator==(const BuildOptions &) const noexcept =
		default;
};

/* Direct control-flow gadgets load these immutable-layout cells through the
 * data stream. The cache may retarget a cell from the shared exit stream to a
 * translated successor without modifying executable memory. */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_pc{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_pc) == 16);
static_assert(sizeof(BlockLink) == 24);

struct IndirectLinkEntry final {
	std::uint64_t guest_pc{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Target cells cover the monomorphic and common polymorphic branch/return
 * sites while keeping the assembly hit path compact: a hit in cell N costs
 * three instructions per cell scanned before it. ProgramCache owns and trains
 * these data-only cells; translated streams only retain stable pointers to
 * them. The count is a layout constant shared with the assembly hit path. */
inline constexpr std::size_t indirect_link_entries = UML_GADGET_INDIRECT_ENTRIES;
static_assert(indirect_link_entries == 2, "the hit path scans two cells");
struct IndirectLink final {
	std::array<IndirectLinkEntry, indirect_link_entries> entries{};
	std::uint8_t next_replacement{};
	/* Diagnostic only: 1 for a return site, 0 for a register branch. */
	std::uint8_t kind{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) ==
	16 * indirect_link_entries);
static_assert(sizeof(IndirectLink) == 16 * indirect_link_entries + 8);

/* An address space's target table (CpuState::gadget_target_tier): its
 * indirect-site misses fill it, and the cached indirect gadgets consult it
 * once a site's cells miss. The index is the target's word address within a
 * 4 KiB page folded with its page number, exactly as the gadget code computes
 * it, so that targets a multiple of 4 KiB apart -- page-aligned functions,
 * handler tables -- do not collide. */
inline constexpr std::size_t target_tier_entries =
	std::size_t{1} << UML_GADGET_TARGET_TIER_BITS;
static_assert(UML_GADGET_TARGET_TIER_BITS == 10,
	"the fold pairs bits 2-11 with bits 12-21");

[[nodiscard]] constexpr std::size_t target_tier_index(std::uint64_t pc) noexcept
{
	return static_cast<std::size_t>(((pc ^ (pc >> 10)) >> 2) &
		(target_tier_entries - 1));
}

class Builder final {
public:
	using LinkResolver =
		std::function<const BlockLink *(std::uint64_t guest_pc)>;
	using IndirectLinkResolver =
		std::function<IndirectLink *(std::uint64_t source_pc)>;

	[[nodiscard]] BuildResult build(
		std::uint64_t guest_address,
		std::span<const std::uint32_t> instructions,
		LinkResolver resolve_link = {},
		std::pmr::memory_resource *storage =
			std::pmr::get_default_resource(),
		IndirectLinkResolver resolve_indirect_link = {},
		BuildOptions options = {}) const;
};

[[nodiscard]] ::uml::cpu::EngineEvent execute(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept;
[[nodiscard]] ::uml::cpu::EngineEvent execute(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept;
[[nodiscard]] ::uml::cpu::EngineEvent
execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept;
/* execute_preserving_native_fp_environment for a program translated for
 * SVCR.SM = 1, which runs with the host in streaming mode. The host must
 * implement SME, and `zt0` says whether the guest has ZT0 to carry. */
[[nodiscard]] ::uml::cpu::EngineEvent
execute_streaming_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed, bool zt0) noexcept;
using ::uml::cpu::arm64::restore_native_fp_environment;

} // namespace uml::cpu::arm64::gadgets
