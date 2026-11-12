// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "cpu/x86_64/layout/assembly_offsets.h"
#include "memory/guest_address_space.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

struct Gadget;
class Executor;

/* A gadget is immutable normalized instruction data. Translation resolves it
 * exclusively to precompiled arm64 assembly entries; no guest instruction is
 * copied into executable memory and no semantic C++ executor remains.
 */
struct Gadget final {
	std::uint64_t address{};
	Instruction instruction{};
};

/* Cached direct branches name stable, data-only link records. A reset link
 * enters its private fallback stream; an active link enters the cached target
 * without modifying executable memory. The target's guest bounds, which
 * preserve x86's self-modifying-code check while a native chain crosses
 * program objects, are the two words in front of whatever stream the link
 * names (stream_header_words): a stored program's own, or the record's
 * {RIP, RIP} in front of its fallback word. A link is therefore retargeted by
 * its {stream, count} pair alone.
 */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_rip{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_rip) == 16);
static_assert(sizeof(BlockLink) == 24);

/* The words in front of every stream a link can name: the guest begin and end
 * of the code it runs, which follow_block_link reads at [stream, #-16]. */
inline constexpr std::size_t stream_header_words = 2;

/* What a link that names no block looks like, and the layout every link
 * record shares with it. The unlinked state's stream is the record's own
 * link_exit word, so {begin, end} = {RIP, RIP} sit in front of that word
 * like any program's header, and link_exit finds the RIP two words past the
 * word it was entered at, which is the BlockLink's own. The {stream, count}
 * pair that retargets a link starts on a sixteen-byte boundary, so that one
 * paired store replaces it whole and a reader's paired load sees one state.
 */
struct alignas(16) UnlinkedBlockLink final {
	std::uint64_t owner;
	std::uint64_t unlinked_begin;
	std::uint64_t unlinked_end;
	std::uintptr_t fallback_stream;
	BlockLink link;
};
static_assert(offsetof(UnlinkedBlockLink, fallback_stream) ==
		offsetof(UnlinkedBlockLink, unlinked_begin) +
			sizeof(std::uint64_t) * stream_header_words &&
	offsetof(UnlinkedBlockLink, link) ==
		offsetof(UnlinkedBlockLink, fallback_stream) +
			sizeof(std::uintptr_t),
	"the bounds precede the fallback word, which precedes the link");
static_assert(offsetof(UnlinkedBlockLink, link) % 16 == 0,
	"the link's stream and count are one aligned pair");

struct IndirectLinkEntry final {
	std::uint64_t guest_rip{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_rip) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Match arm64's monomorphic/bimorphic indirect tier. The translated hit path
 * reads only the two target/link pairs; replacement and live-list state stay
 * in data and never require executable publication. */
struct IndirectLink final {
	std::array<IndirectLinkEntry, 2> entries{};
	std::uint8_t next_replacement{};
	bool listed{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) == 32);
static_assert(offsetof(IndirectLink, listed) == 33);
static_assert(sizeof(IndirectLink) == 40);

/* A return site belongs to the call that reached it, not to the RET that
 * leaves it, so a function called from three places overruns a two-way tier
 * keyed on the branch. One direct-mapped tier keyed on the target instead is
 * shared by every indirect branch in an address space: whatever block ran
 * most recently at that guest RIP is reachable from all of them.
 *
 * An entry is only a prediction. The follower compares the guest RIP it has
 * already computed against the recorded one and enters the link only on a
 * match, so a stale or evicted entry costs a fallback and never a wrong
 * branch. Unused entries name an address no block starts at and carry that
 * address's own record, which keeps `link` dereferenceable without a null
 * test on the translated path.
 */
struct TargetLinkEntry final {
	std::uint64_t guest_rip{};
	const BlockLink *link{};
};
static_assert(offsetof(TargetLinkEntry, guest_rip) == 0);
static_assert(offsetof(TargetLinkEntry, link) == 8);
static_assert(sizeof(TargetLinkEntry) == 16);

/* follow_target_link in engine/common.inc probes with the same fold and
 * mask. */
inline constexpr std::size_t target_link_entries =
	std::size_t{UML_X86_64_TARGET_LINK_MASK} + 1U;
inline constexpr unsigned target_link_fold = UML_X86_64_TARGET_LINK_FOLD;
static_assert((target_link_entries & (target_link_entries - 1U)) == 0,
	"the translated tier masks the guest RIP fold");

/* Block starts cluster inside a page, so fold the page number back over the
 * offset before masking. */
[[nodiscard]] inline constexpr std::size_t target_link_index(
	std::uint64_t guest_rip) noexcept
{
	return static_cast<std::size_t>(
		(guest_rip ^ (guest_rip >> target_link_fold)) &
		(target_link_entries - 1U));
}

using TargetLinkTable = std::array<TargetLinkEntry, target_link_entries>;

struct ProgramLinkResolver final {
	void *context{};
	const BlockLink *(*resolve)(void *, std::uint64_t, ExecutionMode){};
	IndirectLink *(*resolve_indirect)(
		void *, std::uint64_t, ExecutionMode){};
};

/* A translation is two pieces, each at exactly the size the block turned out to
 * need: the native stream the engine runs, behind its guest bounds, and the
 * guest bytes the decoder read. Its decoded instructions are not among them --
 * they are what those bytes decode to, and the only thing that ever asks for
 * them again is a budget-limited replay, which decodes them back out of the
 * source. A block that one replay has ended inside keeps what that decode
 * produced, and nothing else does. Keeping the streams apart from the source
 * is what keeps them near each other in the allocator, which the workloads
 * with the widest code notice.
 *
 * A program a build returns owns its pieces. One the cache stores is a view:
 * its stream and source belong to the body that holds it (the cache's arena),
 * which outlives every use of the view, and only what a replay decoded is the
 * program's own to free. A viewing program is never copied or moved out of its
 * body. */
class Program final {
public:
	Program() = default;
	/* `source` is the guest bytes the decoder read for `gadgets`, which is
	 * what the block is validated against and what a budget-limited replay
	 * decodes back out of, and `words` is the native stream built for
	 * them. */
	Program(ExecutionMode mode, std::uint64_t address,
		std::span<const Gadget> gadgets,
		std::span<const std::byte> source,
		std::span<const std::uintptr_t> words, bool check_head = false);
	~Program();
	Program(Program &&other) noexcept;
	Program &operator=(Program &&other) noexcept;
	Program(const Program &) = delete;
	Program &operator=(const Program &) = delete;

	/* The words a stored program's stream takes with its header, and where
	 * its view begins: `stream` is stream_header_words words of guest bounds
	 * followed by the stream, and `source` is a copy of the saved bytes, both
	 * already in the storage the view will live in. */
	[[nodiscard]] static std::size_t stream_bytes(
		std::size_t words) noexcept
	{
		return (stream_header_words + words) * sizeof(std::uintptr_t);
	}
	/* Fill `stream` (stream_bytes(words.size()) bytes, aligned to sixteen)
	 * with the guest bounds and the words, and `source` (source.size() bytes)
	 * with the saved bytes, and return the view that stands for them. */
	[[nodiscard]] Program view_in(std::uintptr_t *stream, std::byte *source)
		const noexcept;

	[[nodiscard]] ExecutionMode execution_mode() const noexcept { return mode_; }
	[[nodiscard]] std::uint64_t address() const noexcept { return address_; }
	/* The guest bytes the block spans, which are the bytes its source saves:
	 * the instructions kept, one encoding after another. */
	[[nodiscard]] std::size_t byte_count() const noexcept { return source_; }
	[[nodiscard]] std::span<const std::uintptr_t> native_stream() const noexcept
	{
		return {words_ + stream_header_words, words_count_};
	}
	/* The allocation holding the stream and its header. */
	[[nodiscard]] const void *stream_allocation() const noexcept
	{
		return words_;
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return instructions_;
	}
	/* Whether the block ends at an instruction that transfers control. A
	 * block that does not is left by falling off its end, which is what the
	 * cache's successor chain needs to know; the program answers it from a
	 * flag rather than from decoded instructions it no longer carries. */
	[[nodiscard]] bool terminal() const noexcept { return terminal_; }
	/* A block built for exceptions the guest enabled whose one instruction
	 * needs a check: its stream only leaves, at that instruction, so the
	 * runtime runs it with `execute_checked` however it was entered. */
	[[nodiscard]] bool check_head() const noexcept { return check_head_; }
	/* The guest bytes the decoder read for this block. A cached translation
	 * and the source that stands for it are one object as well as one
	 * observation, and the block's instructions are recoverable from it
	 * without reading guest memory again. */
	[[nodiscard]] std::span<const std::byte> source() const noexcept
	{
		return {source_bytes_, source_};
	}
	/* This block's decoded instructions, for the budget-limited replay that
	 * is the only caller: a single step, or the one instruction a POPF that
	 * sets TF leaves, which are the only exact budgets. The first one
	 * decodes them out of the saved source -- the same bytes the build read,
	 * so the block recovers as itself whatever the guest has done to that
	 * memory since -- and the program keeps them, because a block that is
	 * stepped through once is usually stepped through again. An empty
	 * result means the decode or its storage failed and the caller has
	 * nothing to run.
	 *
	 * Executors that share the program may ask at once: each decodes into
	 * storage of its own and the first to publish keeps its copy. */
	[[nodiscard]] std::span<const Gadget> replay_instructions() const noexcept;
	/* What those instructions cost while they are resident, which is nothing
	 * until a replay has asked for them. */
	[[nodiscard]] std::size_t replay_bytes() const noexcept
	{
		return decoded_.load(std::memory_order_relaxed) == nullptr ? 0 :
			std::size_t{instructions_} * sizeof(Gadget);
	}

private:
	void release() noexcept;

	/* Widest members first: every cached translation embeds a program, so
	 * padding here is paid once per translation. */
	std::uint64_t address_{};
	/* The words the engine runs behind their guest bounds, alone. Holding
	 * them in one allocation with the cold data behind them put every hot
	 * stream a cold kilobyte or two away from the next and cost the workloads
	 * with the widest code. */
	std::uintptr_t *words_{};
	/* The guest bytes the decoder read, untouched while the block runs. */
	std::byte *source_bytes_{};
	/* What one budget-limited replay decoded, kept for the next one. Null
	 * until a replay asks, which most blocks never do. */
	mutable std::atomic<std::byte *> decoded_{};
	std::uint32_t words_count_{};
	std::uint32_t instructions_{};
	std::uint32_t source_{};
	ExecutionMode mode_{ExecutionMode::long64};
	bool terminal_{};
	bool check_head_{};
	/* Whether the stream and source are this program's to free. */
	bool owns_{};
};
static_assert(sizeof(Program) == 48,
	"every cached translation embeds a program");

enum class BuildStatus : std::uint8_t {
	built,
	fetch_fault,
	illegal_instruction,
	allocation_failure,
};

struct BuildResult final {
	BuildStatus status{BuildStatus::illegal_instruction};
	Program program{};
	std::uint64_t fault_address{};
};

[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions = 64) noexcept;
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions = 64) noexcept;
/* Every program carries the bytes the decoder read for the instructions the
 * block kept, in address order. Saving those rather than reading the block
 * again is what makes a program and its saved source one observation of guest
 * memory instead of two, and it is also the block's instructions: anything
 * that needs them decodes them back. */
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links) noexcept;
/* A build for a guest that has enabled the exceptions in `checked`, which
 * `checked_exceptions` reports: the block ends before the first instruction
 * that could raise one, and a block that starts with such an instruction
 * holds it alone and is a check head. Every block of a cache built this way
 * is safe to link to under `checked` or any part of it. */
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links,
	unsigned checked) noexcept;

/* The work, in elements or 64-byte blocks, that a repeated string
 * instruction does before it stops between elements. */
inline constexpr std::size_t string_repeat_budget =
	UML_X86_64_STRING_REPEAT_BUDGET;

struct ExecuteResult final {
	::uml::cpu::EngineEvent event{::uml::cpu::EngineEvent::fatal};
	std::size_t retired_instructions{};
	::uml::cpu::FaultInfo fault{};
	bool instruction_budget_exhausted{};
	/* A continued run that ended in a repeated string instruction stopped
	 * between elements after `string_repeat_budget` of work. It retired
	 * nothing for that work, and RIP still names it. */
	bool repeat_yielded{};
	/* A floating-point trap's x86 vector: #DE, 0, from the engine, or #MF,
	 * 16, or #XM, 19, from `execute_checked`, whose `error` is then the
	 * x87 status word with the control word above it, or MXCSR. A
	 * general-protection event's #NP, #SS or #GP from the segment model,
	 * 11, 12 or 13, and its error code; zero for the engine's own #GP. */
	std::uint8_t vector{};
	std::uint32_t error{};
	/* A continued run stopped at an instruction whose access through a
	 * segment it could not check itself, which RIP names and which has not
	 * retired: the runtime checks it exactly (`prepare_segment_step`). */
	bool segment_check{};
};

[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {}) noexcept;
/* With `single_step` a repeated string instruction at the block's start
 * runs one iteration, as x86 raises #DB after each: it retires if that
 * iteration ends it and otherwise leaves between elements, a repeat yield
 * with RF set. */
[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {},
	bool single_step = false) noexcept;
/* The step of the instruction RIP names through the segment model: its
 * exact check (`prepare_segment_step`), then the run of it alone under a
 * SegmentBypass, or what the model did instead. A task whose ES, SS or DS
 * has a base or a limit runs every instruction this way; `execute` steps an
 * instruction that failed its inline check or guard itself. */
[[nodiscard]] ExecuteResult segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	const ::uml::cpu::InterruptToken &interrupt, unsigned checked,
	std::uint64_t string_elements) noexcept;
/* segment_step after a run that `prior` describes stopped at the check. */
[[nodiscard]] ExecuteResult continue_segment_step(const ExecuteResult &prior,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	Executor &executor, const ::uml::cpu::InterruptToken &interrupt,
	unsigned checked, bool single) noexcept;
/* Runs `gadget`, an instruction RIP names, alone: the step of a task whose
 * segments the engine's address generation cannot honour on its own, or of
 * an instruction whose segment access the runtime has just checked
 * (`prepare_segment_step`). The caller holds a SegmentBypass. */
[[nodiscard]] ExecuteResult execute_step(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	const ::uml::cpu::InterruptToken &interrupt, unsigned checked) noexcept;
/* `execute` while the guest has enabled an exception the engine does not
 * check for: `checked` is `checked_exceptions(cpu)`, which is not zero, and
 * the program comes from a build for `checked` or more. A block without a
 * check head runs as `execute` runs it. A check head's instruction runs
 * alone, with its checks: #MF for a waiting x87 instruction with an unmasked
 * exception pending, #AC for a misaligned access, and after it #XM, or the
 * pending #MF an x87 instruction leaves for the next one. */
[[nodiscard]] ExecuteResult execute_checked(const Program &program,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	Executor &executor, std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt, bool single_step,
	unsigned checked) noexcept;

} // namespace uml::cpu::x86_64::gadgets
