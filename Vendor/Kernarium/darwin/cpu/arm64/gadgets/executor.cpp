// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/arm64/aarch32/breakpoint_immediate.hpp"
#include "cpu/arm64/aarch32/guest_fetch.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/guest_validation.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/gadgets/a32/a32_builder.hpp"
#include "cpu/gadget_body_arena.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_generation_window.hpp"
#include "cpu/gadget_record_table.hpp"
#include "cpu/gadget_shared_space.hpp"
#if UML_GADGET_CACHE_CENSUS
#include "cpu/gadget_cache_census.hpp"
#endif
#include "uml_darwin_static_mmu.h"
#include "memory/domain_heap.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/mman.h>

namespace uml::cpu::arm64::gadgets {
using ::uml::memory::StaticGuestMemory;
#if UML_GADGET_CACHE_CENSUS
namespace census = ::uml::cpu::gadget_cache::census;
#endif
namespace {


/* darwin/cpu/CMakeLists.txt defines it for this translation unit, the only
 * one that reads it. */
#ifndef UML_GADGET_BLOCK_INSTRUCTIONS
#error "UML_GADGET_BLOCK_INSTRUCTIONS must come from the build"
#endif
/* Set only by the unit-test variant of this file (see the stress hooks at its
 * end), which renews shared address spaces on a schedule instead of waiting
 * for their freed arena to grow. */
#ifndef UML_GADGET_SWEEP_STRESS
#define UML_GADGET_SWEEP_STRESS 0
#endif
constexpr std::size_t block_instruction_count = UML_GADGET_BLOCK_INSTRUCTIONS;
constexpr std::size_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;
/* A T32 translation validates halfwords: two for each of its instructions,
 * and the two a stopped build read without retiring. */
constexpr std::size_t maximum_t32_validation_units =
	2 * block_instruction_count + 2;

/* A translation cache slot's tag holds the translation's key in its low 48
 * bits, under the id of the address space that owns it. An A64 key is a user
 * PC below the guest address limit (2^47, DARWIN_UML_GUEST_ADDRESS_LIMIT) and
 * is kept as it is; an AArch32 key keeps its 34 significant low bits under
 * two flags in the bits a PC of that width never uses, so no two canonical
 * keys share a compact form. A key outside both has none: it tags to a value
 * no slot holds, which is a miss. */
constexpr std::uint64_t tag_key_mask =
	(std::uint64_t{1} << gadget_cache::tag_owner_shift) - 1U;
constexpr unsigned a64_key_bits = 47;
constexpr std::uint64_t aarch32_flag = std::uint64_t{1} << a64_key_bits;
constexpr std::uint64_t thumb_flag = std::uint64_t{1} << (a64_key_bits - 1U);
constexpr std::uint64_t aarch32_address_mask =
	(std::uint64_t{1} << 34) - 1U;
constexpr std::uint64_t unmatchable_tag = ~std::uint64_t{};

[[nodiscard]] constexpr std::uint64_t compact_tag_key(
	std::uint64_t key) noexcept
{
	if ((key >> a64_key_bits) == 0U) [[likely]]
		return key;
	if (!arm64::aarch32::key_is_canonical(key) ||
	    !arm64::aarch32::key_is_aarch32(key))
		return unmatchable_tag;
	return aarch32_flag |
		(arm64::aarch32::key_is_thumb(key) ? thumb_flag : 0U) |
		(key & aarch32_address_mask);
}

[[nodiscard]] constexpr std::uint64_t expand_tag_key(
	std::uint64_t tag) noexcept
{
	tag &= tag_key_mask;
	if ((tag & aarch32_flag) == 0U)
		return tag;
	return arm64::aarch32::aarch32_key_bit |
		((tag & thumb_flag) != 0U ? arm64::aarch32::thumb_key_bit : 0U) |
		(tag & aarch32_address_mask);
}

static_assert(expand_tag_key(compact_tag_key(0x0000'7fff'ffff'fffcU)) ==
	0x0000'7fff'ffff'fffcU);
static_assert(expand_tag_key(compact_tag_key(0)) == 0);
static_assert(expand_tag_key(compact_tag_key(
	arm64::aarch32::a32_key(0xffff'fffcU))) ==
	arm64::aarch32::a32_key(0xffff'fffcU));
static_assert(expand_tag_key(compact_tag_key(
	arm64::aarch32::t32_key(0xffff'fffeU))) ==
	arm64::aarch32::t32_key(0xffff'fffeU));
static_assert(compact_tag_key(arm64::aarch32::a32_key(0x1000U)) !=
	compact_tag_key(arm64::aarch32::t32_key(0x1000U)));
static_assert(compact_tag_key(0x0000'8000'0000'1000U) == unmatchable_tag);
static_assert(compact_tag_key(arm64::aarch32::a32_key(0x1002U)) ==
	unmatchable_tag);

/* The guest units of a translation window starting at `address`, fetched a
 * page at a time, in units of `unit` bytes (a word for A64 and A32, a
 * halfword for T32). A page that cannot be fetched ends the window; none at
 * all is zero. */
[[nodiscard]] std::size_t fetch_units(StaticGuestMemory &memory,
	std::uint64_t address, std::size_t unit, std::span<std::byte> bytes)
{
	return arm64::aarch32::fetch_guest_units(address, unit, bytes,
		guest_page_size,
		[&memory](std::uint64_t at, std::span<std::byte> out) {
			return memory.fetch(at, out);
		});
}

[[nodiscard]] std::size_t fetch_instruction_window(
	StaticGuestMemory &memory, std::uint64_t pc,
	std::span<std::uint32_t> instructions)
{
	return fetch_units(memory, pc, sizeof(std::uint32_t),
		std::as_writable_bytes(instructions));
}

/* A guest fetch failed: report the fault it left in `memory` as the memory
 * fault of the block that wanted the bytes. */
[[nodiscard]] inline ::uml::cpu::EngineEvent fetch_fault(
	::uml::memory::Tlb &tlb, StaticGuestMemory &memory)
{
	const auto fault = memory.last_fault();
	tlb.fault_address = fault.address;
	tlb.fault_access = ::uml::memory::translated_access(fault.access);
	return ::uml::cpu::EngineEvent::memory_fault;
}

/* A T32 instruction whose second halfword lies on a page that cannot be
 * fetched: the fetch fault is at that halfword, `address`. */
[[nodiscard]] inline ::uml::cpu::EngineEvent fetch_fault_after(
	::uml::memory::Tlb &tlb, StaticGuestMemory &memory,
	std::uint64_t address)
{
	std::uint16_t halfword = 0;
	(void)memory.fetch(address,
		std::as_writable_bytes(std::span{&halfword, 1}));
	return fetch_fault(tlb, memory);
}

void correct_terminal_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	std::uint64_t fault_pc, std::size_t &executed) noexcept
{
	if (!terminal_does_not_retire(event))
		return;

	/* program_position answers only inside the program, so the position
	 * is always below its instruction count. */
	const auto position = program_position(program, fault_pc);
	if (!position)
		return;
	const auto unretired = program.instruction_count() - *position;
	if (executed < unretired)
		return;

	/* The assembly engine precharges a block's full translated length.  A
	 * non-retiring terminal stores its own PC, allowing the cold C++ boundary
	 * to subtract the current block's unexecuted suffix. Counts accumulated
	 * before a linked successor are retained.
	 */
	executed -= unretired;
}

void correct_fpcr_enable_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	const arm64::CpuState &cpu, std::size_t &executed) noexcept
{
	if (event != ::uml::cpu::EngineEvent::continued ||
	    (cpu.effective_fpcr() &
	     arm64::detail::fpcr_exception_enable_mask) == 0U)
		return;

	/* The exit is the fallthrough of the FPCR write, so it is either inside
	 * the program or one past its end. */
	std::size_t retired = 0;
	if (const auto position = program_position(program, cpu.pc); position) {
		retired = *position;
	} else {
		if (cpu.pc != program.guest_address() +
				std::uint64_t{program.instruction_count()} *
				sizeof(std::uint32_t))
			return;
		retired = program.instruction_count();
	}
	if (retired == 0)
		return;
	const auto unretired = program.instruction_count() - retired;
	if (executed < unretired)
		return;

	/* An FPCR write that newly enables traps exits at its translated
	 * fallthrough PC. It retires, but later words in the precharged block do
	 * not. Unresolved control-flow exits cannot satisfy the enabled-state
	 * condition because enabled entries use the one-instruction cold path.
	 */
	executed -= unretired;
}

} // namespace

constexpr std::size_t uml_gadget_cache_slots = ::uml::cpu::gadget_cache::slots;
/* How many address spaces may hold translations at once.
 *
 * Sharing one pool without a bound is not a cache: a guest that starts and
 * exits processes accumulates every address space it ever ran, and the handful
 * that are actually live end up with a few dozen slots each. Reuse measured on
 * a browser guest returns to one of the last eight distinct address spaces
 * 94.5% of the time, so bounding residency there keeps capacity for the ones
 * still running while costing almost no returns.
 */
constexpr std::size_t uml_gadget_resident_generations =
	::uml::cpu::gadget_cache::resident_generations;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

namespace {

using Announcement = gadget_cache::ExecutorAnnouncement;
using gadget_cache::BodyArena;
using gadget_cache::acquire_announcement;
using gadget_cache::announce_entry;
using gadget_cache::announce_exit;
using gadget_cache::release_announcement;
using gadget_cache::store_pair;
using gadget_cache::word_of;

/* One translation, allocated apart from the slots that index it.
 *
 * A set way exists whether or not it holds a translation, so every byte kept
 * inline is paid for by the whole cache. With the program, its stream and its
 * validation words in one separately allocated body, an empty way costs a tag
 * rather than a full BuildResult.
 *
 * The body is also paid for by every translation the cache holds, so it keeps
 * only what a stored block cannot recompute. A BuildResult carries a words
 * object able to own and grow its storage, against a mean stream of about 225
 * bytes; a stored block's words are the ones behind this header. result()
 * rebuilds the builder's type from these fields whenever a caller needs one,
 * which is once per entry from C++.
 */
struct Body final {
	/* Where the block starts: its guest PC, or for AArch32 its key. */
	std::uint64_t guest_address{};
	/* The words of the stream, and the facts about it a stored block
	 * cannot recompute, packed into one word. */
	std::uint32_t word_count : 24 {};
	bool complete : 1 {};
	bool accesses_fp_state : 1 {};
	/* A tail after the validation units holds the build's stop (see
	 * Stop). */
	bool has_stop : 1 {};
	/* The translation's aarch32::GuestIsa, from its key: it sizes a unit
	 * and so the validation words. */
	std::uint32_t isa : 2 {};
	/* How many cache slots, in every executor running the address space,
	 * hold this body. Changed only under its link record's shard lock. */
	std::uint16_t holders{};
	std::uint8_t instruction_count{};
	/* The guest units the stream is validated against: words for A64 and
	 * A32, halfwords for T32 (at most maximum_t32_validation_units). */
	std::uint8_t source_word_count{};

	/* Where a build stopped, which a stored block keeps only when
	 * something can ask for it: a block that stopped at a word it could
	 * not translate, or that holds no instruction, reports its stop
	 * address as the illegal exit's PC. A complete block's terminal is in
	 * its stream and its validation words, and no caller of a stored
	 * block reads the stop fields of one. */
	struct Stop final {
		std::uint64_t address;
		std::uint32_t instruction;
		std::uint32_t padding;
	};

	/* The stream follows the body, and the guest words it is validated
	 * against follow the stream. A block that accesses FP/SIMD state is
	 * linked to through one fp_link_entry word in front of its stream, so
	 * its link record needs no prefix of its own. */
	[[nodiscard]] std::size_t entry_words() const noexcept
	{
		return accesses_fp_state ? 1U : 0U;
	}
	[[nodiscard]] const std::uintptr_t *link_entry() const noexcept
	{
		return reinterpret_cast<const std::uintptr_t *>(this + 1);
	}
	[[nodiscard]] std::uintptr_t *words() noexcept
	{
		return reinterpret_cast<std::uintptr_t *>(this + 1) +
			entry_words();
	}
	[[nodiscard]] const std::uintptr_t *words() const noexcept
	{
		return link_entry() + entry_words();
	}
	[[nodiscard]] std::uint32_t *source_words() noexcept
	{
		return reinterpret_cast<std::uint32_t *>(words() + word_count);
	}
	[[nodiscard]] const std::uint32_t *source_words() const noexcept
	{
		return reinterpret_cast<const std::uint32_t *>(
			words() + word_count);
	}
	/* The validation units are bytes in a unit the ISA sizes: a word, or
	 * for T32 a halfword. */
	[[nodiscard]] std::size_t source_unit() const noexcept
	{
		return arm64::aarch32::isa_unit_bytes(
			static_cast<arm64::aarch32::GuestIsa>(isa));
	}
	[[nodiscard]] std::byte *source_bytes() noexcept
	{
		return reinterpret_cast<std::byte *>(source_words());
	}
	[[nodiscard]] const std::byte *source_bytes() const noexcept
	{
		return reinterpret_cast<const std::byte *>(source_words());
	}
	[[nodiscard]] std::span<const std::byte> source() const noexcept
	{
		return {source_bytes(),
			std::size_t{source_word_count} * source_unit()};
	}
	/* Whether a build's stop is kept: see Stop. */
	[[nodiscard]] static bool keeps_stop(
		bool complete, std::size_t instruction_count) noexcept
	{
		return !complete || instruction_count == 0;
	}
	/* The offset of the stop tail from the body, at its own alignment
	 * after the validation units. */
	[[nodiscard]] static std::size_t stop_offset(
		std::size_t words, std::size_t source_bytes) noexcept
	{
		return (sizeof(Body) + words * sizeof(std::uintptr_t) +
			source_bytes + alignof(Stop) - 1U) &
			~(alignof(Stop) - 1U);
	}
	[[nodiscard]] Stop *stop() noexcept
	{
		return reinterpret_cast<Stop *>(
			reinterpret_cast<std::byte *>(this) +
			stop_offset(entry_words() + word_count,
				source_word_count * source_unit()));
	}
	[[nodiscard]] const Stop *stop() const noexcept
	{
		return const_cast<Body *>(this)->stop();
	}
	/* What the arena must be given back follows from the counts, so
	 * neither the body nor the slot that indexes it stores it. `words`
	 * counts an FP block's entry word with its stream. */
	[[nodiscard]] static std::size_t allocation_bytes(
		std::size_t words, std::size_t source_bytes,
		bool with_stop) noexcept
	{
		return with_stop ?
			stop_offset(words, source_bytes) + sizeof(Stop) :
			sizeof(Body) + words * sizeof(std::uintptr_t) +
				source_bytes;
	}
	[[nodiscard]] std::size_t allocation_bytes() const noexcept
	{
		return allocation_bytes(entry_words() + word_count,
			source_word_count * source_unit(), has_stop);
	}
	/* The stored block as the builder produced it. The program refers to
	 * the words behind this header rather than copying them, so it is
	 * valid exactly as long as the body is. */
	[[nodiscard]] BuildResult result() const noexcept
	{
		arm64::gadgets::ProgramWords view{
			arm64::gadgets::ProgramWords::Borrowed{
				const_cast<std::uintptr_t *>(words()),
				word_count}};
		const auto *kept = has_stop ? stop() : nullptr;
		return BuildResult{arm64::gadgets::Program{guest_address,
				instruction_count, std::move(view),
				accesses_fp_state},
			complete, kept != nullptr ? kept->address : 0U,
			kept != nullptr ? kept->instruction : 0U};
	}
};
static_assert(std::is_trivially_destructible_v<Body>,
	"an address space's bodies go with its arena without being freed");
static_assert(sizeof(Body) == 16 &&
	sizeof(Body) % alignof(std::uintptr_t) == 0,
	"the stream tail follows the body at pointer alignment");
static_assert(maximum_t32_validation_units < 0x100U,
	"a T32 translation's halfword count fits the narrow field");
static_assert(block_instruction_count < 0xffU,
	"a stored block's counts fit the body's narrow fields");

/* A link names a guest PC, which is only meaningful inside one address
 * space; keying by PC alone would let a block in one branch straight into
 * another's translation. Each address space therefore owns its own records,
 * which also keeps the key a single word.
 *
 * Translated code follows `link` with one `ldp` of its stream and count while
 * other vCPUs of the address space may retarget it, so the pair sits on a
 * sixteen-byte boundary and is only ever written whole (store_pair). An
 * unlinked record's stream is the link_exit word right in front of it, which
 * link_exit runs with its stream two words before the PC. Records are carved
 * thirty-two bytes apart, so that word is the last of the record before, or for
 * a chunk's first record the last of the chunk's prefix: each record's last
 * word holds link_exit for the next, and a released record keeps it. Only what
 * generated code reads is in the record; what the cache alone needs, the body
 * a miss may adopt and whether the record is listed, is its companion (see
 * gadget_cache::StableRecordTable).
 */
struct alignas(16) LinkRecord final {
	explicit LinkRecord(std::uint64_t pc) noexcept
		: link{
			.stream = fallback(),
			.instruction_count = 0,
			.guest_pc = pc,
		  },
		  next_fallback{exit_word()}
	{
	}

	void reset() noexcept
	{
		store_pair(&link, word_of(fallback()), 0);
	}

	/* Retarget the record at a stored body. Its words were written before
	 * this, and a vCPU that loads the pair reads them through it. */
	void link_to(const Body &body) noexcept
	{
		std::atomic_thread_fence(std::memory_order_release);
		store_pair(&link, word_of(body.link_entry()),
			body.instruction_count);
	}

	[[nodiscard]] bool names(const Body &body) const noexcept
	{
		return link.stream == body.link_entry();
	}

	/* An unlinked record's stream: the word in front of it, link_exit. */
	[[nodiscard]] const std::uintptr_t *fallback() const noexcept
	{
		return reinterpret_cast<const std::uintptr_t *>(this) - 1;
	}

	[[nodiscard]] static std::uintptr_t exit_word() noexcept
	{
		return reinterpret_cast<std::uintptr_t>(
			uml_arm64_gadget_link_exit);
	}

	arm64::gadgets::BlockLink link;
	/* The next record's fallback, link_exit, whatever that record holds. */
	std::uintptr_t next_fallback;

	/* The chunk's prefix ends in its first record's fallback. */
	static constexpr std::size_t chunk_prefix = 16;
	static void prepare_chunk(std::byte *storage) noexcept
	{
		const auto word = exit_word();
		std::memcpy(storage + chunk_prefix - sizeof(word), &word,
			sizeof(word));
	}
	/* A released record keeps its next record's fallback. */
	static constexpr std::size_t cleared_bytes =
		sizeof(arm64::gadgets::BlockLink);

	/* The newest body stored for this PC that some slot still holds, which
	 * another executor's miss may adopt instead of translating, with bit 0
	 * set while the record sits in its shard's live list, so a code change
	 * resets the records that hold an activation instead of every record
	 * the translator ever named. Bodies are sixteen-byte aligned. */
	struct Companion final {
		std::uintptr_t word{};

		[[nodiscard]] Body *indexed() const noexcept
		{
			return reinterpret_cast<Body *>(
				word & ~std::uintptr_t{1});
		}
		void set_indexed(Body *body) noexcept
		{
			const auto address =
				reinterpret_cast<std::uintptr_t>(body);
			assert((address & 1U) == 0);
			word = address | (word & 1U);
		}
		[[nodiscard]] bool listed() const noexcept
		{
			return (word & 1U) != 0;
		}
		void set_listed(bool value) noexcept
		{
			word = (word & ~std::uintptr_t{1}) | (value ? 1U : 0U);
		}
	};
};

static_assert(offsetof(LinkRecord, link) == 0 &&
	offsetof(LinkRecord, next_fallback) ==
		sizeof(arm64::gadgets::BlockLink) &&
	sizeof(LinkRecord) == 32 &&
	LinkRecord::chunk_prefix % alignof(LinkRecord) == 0,
	"link_exit reads the PC two words past its stream word, the stream and "
	"count are one aligned pair, and each record ends in the next one's "
	"fallback");

struct LinkKey final {
	[[nodiscard]] std::uint64_t operator()(
		const LinkRecord &record) const noexcept
	{
		return record.link.guest_pc;
	}
};

/* An indirect site is named by the branch that owns it, which its own cells do
 * not record. Translated streams point at the site, the first member; its
 * cells are aligned pairs that translated code trains as it runs. */
struct alignas(16) IndirectRecord final {
	explicit IndirectRecord(std::uint64_t source) noexcept
		: source_pc{source}
	{}

	arm64::gadgets::IndirectLink site{};
	std::uint64_t source_pc;
};

static_assert(offsetof(IndirectRecord, site) == 0 &&
	sizeof(IndirectRecord) == 48,
	"indirect cells are aligned pairs");

struct IndirectKey final {
	[[nodiscard]] std::uint64_t operator()(
		const IndirectRecord &record) const noexcept
	{
		return record.source_pc;
	}
};

/* Executors running one address space share its translations unless
 * DARWIN_UML_GADGET_SHARED_BODIES=0 keeps every executor's to itself. A
 * shared space bounds the freed arena it accumulates under process churn by
 * renewal, which is what made sharing the default (G227, G267, G268). Read as
 * each executor is made. */
[[nodiscard]] bool shared_bodies_enabled() noexcept
{
#ifdef UML_APP_STORE_RUNTIME
	return true;
#else
	const char *value = std::getenv("DARWIN_UML_GADGET_SHARED_BODIES");
	return value == nullptr || std::strcmp(value, "0") != 0;
#endif
}

/* A shared address space whose arena holds at least this many bytes that no
 * slot needs, and more of them than its bodies hold, is renewed (see
 * Space::renewal_due). */
constexpr std::size_t renewal_floor_bytes = std::size_t{2} << 20;

#if UML_GADGET_SWEEP_STRESS
/* The unit-test variant lowers the floor and renews on a schedule. */
std::atomic<std::size_t> stress_renewal_floor{renewal_floor_bytes};
std::atomic<unsigned> stress_renewal_period{64};
std::atomic<std::size_t> stress_live_spaces{};
std::atomic<std::uint64_t> stress_closes{};
#endif

[[nodiscard]] inline std::size_t renewal_floor() noexcept
{
#if UML_GADGET_SWEEP_STRESS
	return stress_renewal_floor.load(std::memory_order_relaxed);
#else
	return renewal_floor_bytes;
#endif
}

/* What makes two executors' translations interchangeable: the address space,
 * the SVCR mode they were built for and the build options. A key with an
 * owner is private to that cache. */
struct SpaceKey final {
	unsigned long generation{};
	std::uint8_t mode{};
	const void *owner{};
	BuildOptions options{};
};

/* One address space's translations.
 *
 * Its bodies, the link records and indirect sites their streams name, and the
 * target table the indirect gadgets consult all belong to the address space,
 * so every executor running it shares them when the host can retarget a pair
 * atomically. Each executor keeps its own tagged slots, which decide what it
 * holds; a body lives while any slot holds it, so the address space costs the
 * union of what its executors hold rather than the sum. A miss on a body
 * another executor holds adopts it after exact word validation, with no copy
 * or relocation (G161, G166).
 *
 * The records are split eight ways by key, each part behind its own lock, so
 * vCPUs resolving links for different code rarely meet. Lock order: registry,
 * reset, shard, limbo, arena; the registry's lock also covers a space's
 * attached list.
 *
 * A shared address space also counts what its arena holds and what its bodies
 * use of it. Once the arena holds at least as much freed space as bodies
 * (renewal_due), the space is closed: it leaves the registry, so the next
 * executor to want its address space gets a new one, and every executor
 * attached lets go of it before its next entry into translated code. The old
 * space then dies with its last executor and takes its arena, its link
 * records and its freed bodies with it, which window retirement does for a
 * private one.
 *
 * Every space, shared or private, charges its arena, its record tables and its
 * target table to an account in the process's gadget-cache budget
 * (gadget_cache_budget.hpp). When the budget asks it to shed, it asks every
 * executor attached to let go of it, as closing does; the first to do so
 * closes it, and no executor attaches to it any more.
 */
class Space final {
public:
	explicit Space(SpaceKey key, std::uint64_t changes)
		: key_{std::move(key)}, gate_{changes}, reset_done_{changes},
		  bodies_{std::pmr::new_delete_resource()}, account_{shed, this}
	{
		bodies_.meter(&account_);
		for (auto &shard : shards_) {
			shard.links.meter(&account_);
			shard.indirect.meter(&account_);
		}
		auto &empty = shard_of(empty_pc);
		empty_link_ = &empty.links.try_emplace(empty_pc, empty_pc)
			.first->link;
#if UML_GADGET_SWEEP_STRESS
		stress_live_spaces.fetch_add(1, std::memory_order_relaxed);
#endif
	}

	/* No executor holds this address space any more, so nothing can run a
	 * body or follow a record of it, and the bodies still waiting in limbo
	 * go with the arena: it unmaps its chunks whole, which costs what it
	 * mapped rather than what it holds. Freeing each body first merged
	 * and listed blocks the next step discards. See G257. */
#if UML_GADGET_SWEEP_STRESS
	/* The unit-test variant counts the address spaces alive. */
	~Space() { stress_live_spaces.fetch_sub(1, std::memory_order_relaxed); }
#else
	~Space() = default;
#endif

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	/* The unreachable guest PC an untrained indirect cell names; guest
	 * targets are admitted only word-aligned, so none can collide. */
	static constexpr std::uint64_t empty_pc = ~std::uint64_t{};

	[[nodiscard]] const SpaceKey &key() const noexcept { return key_; }

	void attach(const Announcement *announcement)
	{
		attached_.attach(announcement);
	}

	/* Whether this was the last executor. */
	[[nodiscard]] bool detach(const Announcement *announcement)
	{
		return attached_.detach(announcement);
	}

	/* Whether this shared address space holds enough freed arena to be
	 * replaced: at least the floor, and as much as its bodies use. A private
	 * one never is, since its executor's window retires it. */
	[[nodiscard]] bool renewal_due() const noexcept
	{
		return ledger_.renewal_due();
	}

	[[nodiscard]] bool closed() const noexcept
	{
		return ledger_.closed();
	}

	/* Whether the gadget-cache budget asked this space to shed: an executor
	 * holding it closes it and lets go of it at its next entry into
	 * translated code. */
	[[nodiscard]] bool shedding() const noexcept
	{
		return account_.shedding();
	}

	/* What the space charges the budget. */
	[[nodiscard]] std::size_t charged() const noexcept
	{
		return account_.bytes();
	}

#if UML_GADGET_SWEEP_STRESS
	[[nodiscard]] std::size_t stress_arena_bytes() const noexcept
	{
		return ledger_.arena_bytes();
	}
	[[nodiscard]] std::size_t stress_held_bytes() const noexcept
	{
		return ledger_.held_bytes();
	}
#endif

	/* The registry closed a space: counted by the unit-test variant. */
	static void note_closed() noexcept
	{
#if UML_GADGET_SWEEP_STRESS
		stress_closes.fetch_add(1, std::memory_order_relaxed);
#endif
	}

	/* Close the address space; true for the caller that did, which then
	 * withdraws it from the registry and calls request_renewal(). */
	[[nodiscard]] bool close() noexcept
	{
		return ledger_.close();
	}

	/* Ask every executor attached to let go of this address space before it
	 * next enters translated code. */
	void request_renewal()
	{
		attached_.request_renewal();
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(std::uint64_t pc)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		return &shard.links.try_emplace(pc, pc).first->link;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		auto &shard = shard_of(source_pc);
		const std::lock_guard guard{shard.mutex};
		const auto [created, inserted] =
			shard.indirect.try_emplace(source_pc, source_pc);
		if (inserted)
			for (auto &entry : created->site.entries)
				store_pair(&entry, empty_pc, word_of(empty_link_));
		return &created->site;
	}

	/* The target table, or null before the address space's first indirect
	 * miss. It lives as long as the records it names. */
	[[nodiscard]] const void *target_tier() const noexcept
	{
		return targets_.load(std::memory_order_acquire);
	}

	/* Train `site` and the target table with `target`, the record for
	 * `target_pc`. Every vCPU of the address space misses into here, so
	 * only a site's first training takes a lock.
	 *
	 * A cell or table entry only ever names a record, and a record is
	 * linked only to a body validated at the gate, so writing one without
	 * the shard lock is safe even while a code change resets the site:
	 * following it at worst exits through link_exit. Enrolment in the live
	 * list only keeps a reset site's predictions clean. */
	void learn_indirect(arm64::gadgets::IndirectLink &site,
		std::uint64_t target_pc, const arm64::gadgets::BlockLink *target)
	{
		train_target(target_pc, target);
		/* Training is the only thing C++ does to move a site off its
		 * empty cells, and empty_pc is unreachable as a guest target, so
		 * a site still holding two empty cells is untrained since its
		 * last reset -- which makes this the transition that enrols it.
		 * Translated code trains cells too, without enrolling; such a
		 * cell names a record, which a code change resets. */
		if (untrained(site)) {
			const auto &owner =
				*reinterpret_cast<const IndirectRecord *>(&site);
			auto &shard = shard_of(owner.source_pc);
			const std::lock_guard guard{shard.mutex};
			if (untrained(site)) {
				shard.live_indirect.push_back(&site);
				store_pair(&site.entries[0], target_pc,
					word_of(target));
				return;
			}
		}
		for (auto &entry : site.entries) {
			if (cell_pc(entry) != target_pc)
				continue;
			store_pair(&entry, target_pc, word_of(target));
			return;
		}
		std::atomic_ref<std::uint8_t> replacement{site.next_replacement};
		const auto next = replacement.load(std::memory_order_relaxed);
		store_pair(&site.entries[next %
				arm64::gadgets::indirect_link_entries],
			target_pc, word_of(target));
		replacement.store(static_cast<std::uint8_t>((next + 1U) %
			arm64::gadgets::indirect_link_entries),
			std::memory_order_relaxed);
	}

	/* Store a freshly built translation, or adopt the equal one another
	 * executor stored first, and hold it for the caller's slot. */
	[[nodiscard]] Body *publish(const BuildResult &built,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa)
	{
		const auto pc = built.program.guest_address();
		auto &shard = shard_of(pc);
		{
			const std::lock_guard guard{shard.mutex};
			const auto &record = shard.links.companion_at(
				shard.links.try_emplace_number(pc, pc).first);
			if (auto *held = equal_indexed(record, validation, isa)) {
				++held->holders;
				return held;
			}
		}
		auto *body = new_body(built, validation, isa);
		{
			const std::lock_guard guard{shard.mutex};
			auto &record = shard.links.companion_at(
				shard.links.find_number(pc));
			if (auto *held = equal_indexed(record, validation, isa)) {
				++held->holders;
				/* Never published, so nothing can reach it. */
				const std::lock_guard arena{arena_mutex_};
				delete_body(body);
				return held;
			}
			body->holders = 1;
			if (key_.owner == nullptr)
				note_held(*body);
			if (!validation.empty())
				record.set_indexed(body);
		}
		return body;
	}

	/* Hold the body indexed for `pc`, which the caller will validate. */
	[[nodiscard]] Body *hold_indexed(std::uint64_t pc)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		const auto number = shard.links.find_number(pc);
		if (number == 0)
			return nullptr;
		auto *body = shard.links.companion_at(number).indexed();
		if (body != nullptr)
			++body->holders;
		return body;
	}

	/* A slot lets go of `body`. `stale` says validation found guest code
	 * other than the body's, so no miss should adopt it any more. */
	void release(Body *body, bool stale)
	{
		auto &shard = shard_of(body->guest_address);
		const std::lock_guard guard{shard.mutex};
		const auto number =
			shard.links.find_number(body->guest_address);
		auto *record =
			number != 0 ? shard.links.record_at(number) : nullptr;
		assert(body->holders != 0);
		const bool last = --body->holders == 0;
		if (record != nullptr) {
			auto &companion = shard.links.companion_at(number);
			if (companion.indexed() == body && (last || stale))
				companion.set_indexed(nullptr);
		}
		if (!last)
			return;
		/* Only a body that leaves can leave a record nothing names. */
		if (key_.owner != nullptr)
			++departed_;
		else
			note_released(*body);
		/* Nothing names it from here on; executors that loaded the
		 * record before this may still be running it, so it waits for
		 * them in limbo. */
		if (record != nullptr && record->names(*body))
			record->reset();
		limbo_.push(body);
	}

	/* Link the record for `pc` to `body`, which the caller holds and
	 * validated at `changes`, unless a newer code change has reset the
	 * address space's links since. `known` is the caller's memo of the
	 * record's number, or zero. Returns the number and the link for that
	 * memo. */
	std::pair<std::uint32_t, const arm64::gadgets::BlockLink *> activate(
		std::uint64_t pc, std::uint32_t known, const Body &body,
		std::uint64_t changes)
	{
		auto &shard = shard_of(pc);
		const std::lock_guard guard{shard.mutex};
		const auto number = known != 0 ? known :
			shard.links.try_emplace_number(pc, pc).first;
		auto *record = shard.links.record_at(number);
		assert(record->link.guest_pc == pc);
		if (gate_.load(std::memory_order_seq_cst) != changes)
			return {number, &record->link};
		/* Only an activation links a record, and it lists the record
		 * first; a code change clears both together. So a record that
		 * already names `body` is listed, and its companion, a line of
		 * its own, need not be read. */
		if (record->names(body))
			return {number, &record->link};
		if (auto &companion = shard.links.companion_at(number);
		    !companion.listed()) {
			shard.live_links.push_back(number);
			companion.set_listed(true);
		}
		record->link_to(body);
		return {number, &record->link};
	}

	/* Answer a code change: before any executor runs the address space at
	 * `changes`, every link and site that held an activation is reset.
	 * Raising the gate first means an activation racing this either lands
	 * before its shard is reset or sees the gate and stays unlinked. */
	void observe(std::uint64_t changes)
	{
		if (reset_done_.load(std::memory_order_acquire) >= changes)
			return;
		const std::lock_guard reset{reset_mutex_};
		if (reset_done_.load(std::memory_order_relaxed) >= changes)
			return;
		gate_.store(changes, std::memory_order_seq_cst);
		for (auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			for (const auto number : shard.live_links) {
				auto &companion = shard.links.companion_at(number);
				shard.links.record_at(number)->reset();
				companion.set_listed(false);
			}
			shard.live_links.clear();
			/* The reset cells name this address space's own empty
			 * record, so a site never points at another's link
			 * state. */
			for (auto *site : shard.live_indirect) {
				for (auto &entry : site->entries)
					store_pair(&entry, empty_pc,
						word_of(empty_link_));
				std::atomic_ref<std::uint8_t>{site->next_replacement}
					.store(0, std::memory_order_relaxed);
			}
			shard.live_indirect.clear();
		}
		reset_done_.store(changes, std::memory_order_release);
	}

	using LinkTable = gadget_cache::StableRecordTable<LinkRecord, LinkKey>;
	using SiteTable =
		gadget_cache::StableRecordTable<IndirectRecord, IndirectKey>;

	/* Whether to sweep this address space's link records and sites now
	 * (gadget_cache::sweep_due).
	 *
	 * Only an address space one executor keeps to itself is swept: that
	 * executor's slots are then everything that holds one of its bodies,
	 * and none of its translated code runs while the executor stores. A
	 * shared one would need the slots of every executor holding it, a grace
	 * period like its bodies' before a released record is used again, and
	 * every executor's memos and return ring let go of what went, so it is
	 * never swept. */
	[[nodiscard]] bool sweep_due() const noexcept
	{
		return key_.owner != nullptr &&
			gadget_cache::sweep_due(records(), kept_, departed_);
	}

	/* Marks over every record and site, the empty record already kept. */
	[[nodiscard]] std::pair<LinkTable::Marks, SiteTable::Marks> marks() const
	{
		std::array<const LinkTable *, shard_count> links;
		std::array<const SiteTable *, shard_count> sites;
		for (std::size_t index = 0; index < shard_count; ++index) {
			links[index] = &shards_[index].links;
			sites[index] = &shards_[index].indirect;
		}
		auto result = std::pair{LinkTable::marks_of(links),
			SiteTable::marks_of(sites)};
		result.first.mark(record_of(empty_link_));
		return result;
	}

	/* The record for `pc`, or null; never makes one. */
	[[nodiscard]] const LinkRecord *find_link(std::uint64_t pc) const noexcept
	{
		return shards_[shard_index(pc)].links.find(pc);
	}

	struct Released final {
		std::size_t links;
		std::size_t sites;
	};

	/* Release every record and site the marks leave, which the one
	 * executor holding this address space found nothing to name. A site
	 * cell or target-table entry may still name such a record, whose
	 * block is gone; it is emptied as a code change empties it, to learn
	 * its target again if that runs. Everything this allocates comes before
	 * anything is released, and each step after leaves the address space
	 * consistent, so a sweep that runs out of memory part way only keeps
	 * more than it could have. */
	[[nodiscard]] Released sweep(const LinkTable::Marks &links,
		const SiteTable::Marks &sites)
	{
		assert(key_.owner != nullptr);
		std::array<std::vector<std::uint32_t>, shard_count> live_links;
		std::array<std::vector<arm64::gadgets::IndirectLink *>, shard_count>
			live_indirect;
		for (std::size_t index = 0; index < shard_count; ++index) {
			const auto &shard = shards_[index];
			const std::lock_guard guard{shard.mutex};
			for (const auto number : shard.live_links)
				if (links.marked(shard.links.record_at(number)))
					live_links[index].push_back(number);
			for (auto *site : shard.live_indirect)
				if (sites.marked(site_record(site)))
					live_indirect[index].push_back(site);
		}

		const auto clear = [&](arm64::gadgets::IndirectLinkEntry &entry) {
			if (!links.marked(record_of(entry.link)))
				store_pair(&entry, empty_pc, word_of(empty_link_));
		};
		for (auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.indirect.for_each([&](IndirectRecord &record) {
				for (auto &entry : record.site.entries)
					clear(entry);
			});
		}
		if (auto *table = targets_.load(std::memory_order_acquire))
			for (std::size_t index = 0;
			     index < arm64::gadgets::target_tier_entries; ++index)
				clear(table[index]);
		Released released{};
		for (std::size_t index = 0; index < shard_count; ++index) {
			auto &shard = shards_[index];
			const std::lock_guard guard{shard.mutex};
			/* A site whose cells are all empty again is enrolled by its
			 * next training, as one never trained is. */
			std::erase_if(live_indirect[index],
				[](arm64::gadgets::IndirectLink *site) {
					return untrained(*site);
				});
			shard.live_links = std::move(live_links[index]);
			shard.live_indirect = std::move(live_indirect[index]);
			released.sites += shard.indirect.sweep(sites);
			released.links += shard.links.sweep(links);
		}
		return released;
	}

	/* A sweep, whole or not, has run: the next waits for growth and
	 * departures from here. */
	void swept() noexcept
	{
		kept_ = records();
		departed_ = 0;
	}

	[[nodiscard]] static const LinkRecord *record_of(
		const arm64::gadgets::BlockLink *link) noexcept
	{
		static_assert(offsetof(LinkRecord, link) == 0);
		return reinterpret_cast<const LinkRecord *>(link);
	}

	/* Free what was unpublished long enough ago that no other executor can
	 * still be running it. Never waits. */
	void reclaim(const Announcement *self) noexcept
	{
		limbo_.reclaim(self, attached_,
			[this](std::vector<Body *> &bodies) noexcept {
				free_bodies(bodies);
			});
	}

	/* The live body whose allocation holds `address`, or null. A body
	 * waiting in limbo is still live here, so an exit inside a successor
	 * that was unpublished while it ran is still attributed to it. */
	[[nodiscard]] const Body *owner(const void *address) const
	{
		const std::lock_guard arena{arena_mutex_};
		return static_cast<const Body *>(bodies_.owner(address));
	}

	/* owner() for the guest sampler, which must not wait on a vCPU it
	 * has suspended. */
	[[nodiscard]] bool try_owner(const void *address,
		const Body *&body) const
	{
		std::unique_lock arena{arena_mutex_, std::try_to_lock};
		if (!arena.owns_lock())
			return false;
		body = static_cast<const Body *>(bodies_.owner(address));
		return true;
	}

	struct Footprint final {
		std::size_t stream_index, streams, links, indirect;
	};

	[[nodiscard]] Footprint footprint() const
	{
		Footprint result{};
		{
			const std::lock_guard arena{arena_mutex_};
			result.stream_index = bodies_.index_bytes();
			result.streams = bodies_.bytes();
		}
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			result.links += shard.links.bytes() +
				shard.live_links.capacity() *
					sizeof(std::uint32_t);
			result.indirect += shard.indirect.bytes() +
				shard.live_indirect.capacity() * sizeof(void *);
		}
		if (targets_.load(std::memory_order_relaxed) != nullptr)
			result.indirect += arm64::gadgets::target_tier_entries *
				sizeof(arm64::gadgets::IndirectLinkEntry);
		return result;
	}

#if UML_GADGET_CACHE_CENSUS
	[[nodiscard]] std::size_t census_empty_bytes() const
	{
		const std::lock_guard arena{arena_mutex_};
		return bodies_.empty_bytes();
	}

	[[nodiscard]] std::size_t link_records() const
	{
		std::size_t count = 0;
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			count += shard.links.size();
		}
		return count;
	}

	[[nodiscard]] std::size_t attached() const
	{
		return attached_.size();
	}

	[[nodiscard]] std::size_t limbo_bodies() const
	{
		return limbo_.size();
	}

	template <typename Visit>
	void for_each_link(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.links.for_each(visit);
		}
	}

	template <typename Visit>
	void for_each_indirect(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.indirect.for_each(visit);
		}
	}
#endif

private:
	static constexpr std::size_t shard_count = 8;

	struct Shard final {
		mutable std::mutex mutex;
		/* These grow with the code the address space runs. The records
		 * live in chunks, so each stays where translated link cells name
		 * it, until a sweep of a private address space releases what
		 * nothing names any more (sweep()); a shared one keeps them for
		 * its whole residency. */
		LinkTable links;
		SiteTable indirect;
		/* The records (by number) and sites holding an activation. */
		std::vector<std::uint32_t> live_links;
		std::vector<arm64::gadgets::IndirectLink *> live_indirect;
	};

	[[nodiscard]] static std::size_t shard_index(std::uint64_t key) noexcept
	{
		auto mixed = key * 0x9e3779b97f4a7c15ULL;
		mixed ^= mixed >> 29;
		return (mixed >> 7) & (shard_count - 1U);
	}

	[[nodiscard]] Shard &shard_of(std::uint64_t key) noexcept
	{
		return shards_[shard_index(key)];
	}

	[[nodiscard]] static const IndirectRecord *site_record(
		const arm64::gadgets::IndirectLink *site) noexcept
	{
		return reinterpret_cast<const IndirectRecord *>(
			reinterpret_cast<std::uintptr_t>(site) -
			offsetof(IndirectRecord, site));
	}

	/* Records and sites, which only a private address space's own
	 * executor reads here (sweep_due()). */
	[[nodiscard]] std::size_t records() const noexcept
	{
		std::size_t count = 0;
		for (const auto &shard : shards_)
			count += shard.links.size() + shard.indirect.size();
		return count;
	}

	[[nodiscard]] static std::uint64_t cell_pc(
		arm64::gadgets::IndirectLinkEntry &entry) noexcept
	{
		return std::atomic_ref<std::uint64_t>{entry.guest_pc}.load(
			std::memory_order_relaxed);
	}

	[[nodiscard]] static bool untrained(
		arm64::gadgets::IndirectLink &site) noexcept
	{
		for (auto &entry : site.entries)
			if (cell_pc(entry) != empty_pc)
				return false;
		return true;
	}

	/* The indexed body when it stands for exactly these guest units. */
	[[nodiscard]] static Body *equal_indexed(
		const LinkRecord::Companion &companion,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa) noexcept
	{
		auto *held = companion.indexed();
		if (held == nullptr || validation.empty() ||
		    held->isa != static_cast<std::uint8_t>(isa))
			return nullptr;
		const auto source = held->source();
		if (source.size() != validation.size() ||
		    !std::equal(source.begin(), source.end(),
			    validation.begin()))
			return nullptr;
		return held;
	}

	void train_target(std::uint64_t pc,
		const arm64::gadgets::BlockLink *target)
	{
		using arm64::gadgets::target_tier_entries;
		auto *table = targets_.load(std::memory_order_acquire);
		if (table == nullptr) {
			const std::lock_guard guard{targets_mutex_};
			table = targets_.load(std::memory_order_relaxed);
			if (table == nullptr) {
				targets_storage_ = std::make_unique<
					arm64::gadgets::IndirectLinkEntry[]>(
					target_tier_entries);
				table = targets_storage_.get();
				/* Empty entries name the address space's own
				 * unlinked record for empty_pc, as reset site cells
				 * do. */
				for (std::size_t index = 0;
				     index < target_tier_entries; ++index)
					store_pair(&table[index], empty_pc,
						word_of(empty_link_));
				account_.charge(static_cast<std::ptrdiff_t>(
					target_tier_entries *
					sizeof(arm64::gadgets::IndirectLinkEntry)));
				targets_.store(table, std::memory_order_release);
			}
		}
		store_pair(&table[arm64::gadgets::target_tier_index(pc)], pc,
			word_of(target));
	}

	[[nodiscard]] Body *new_body(const BuildResult &built,
		std::span<const std::byte> validation,
		arm64::aarch32::GuestIsa isa)
	{
		const auto &program = built.program;
		const auto words = program.words();
		assert(program.instruction_count() <= block_instruction_count);
		const auto unit = arm64::aarch32::isa_unit_bytes(isa);
		assert(validation.size() % unit == 0);
		const std::size_t entry = program.accesses_fp_state() ? 1U : 0U;
		const bool with_stop = Body::keeps_stop(
			built.complete, program.instruction_count());
		const auto bytes = Body::allocation_bytes(
			entry + words.size(), validation.size(), with_stop);
		assert(words.size() < (std::size_t{1} << 24));
		void *storage = nullptr;
		bool grown = false;
		{
			const std::lock_guard arena{arena_mutex_};
			storage = bodies_.allocate(bytes);
			if (key_.owner == nullptr)
				grown = note_arena();
		}
		if (grown)
			note_idle();
		auto *body = new (storage) Body{};
		body->guest_address = program.guest_address();
		body->word_count = static_cast<std::uint32_t>(words.size());
		body->instruction_count =
			static_cast<std::uint8_t>(program.instruction_count());
		body->source_word_count =
			static_cast<std::uint8_t>(validation.size() / unit);
		body->complete = built.complete;
		body->accesses_fp_state = program.accesses_fp_state();
		body->has_stop = with_stop;
		body->isa = static_cast<std::uint8_t>(isa) & 3U;
		if (with_stop)
			*body->stop() = {built.stop_address,
				built.stop_instruction, 0U};
		if (entry != 0)
			body->words()[-1] = reinterpret_cast<std::uintptr_t>(
				uml_arm64_gadget_fp_link_entry);
		std::copy(words.begin(), words.end(), body->words());
		std::copy(validation.begin(), validation.end(),
			body->source_bytes());
		return body;
	}

	/* The caller holds arena_mutex_. */
	void delete_body(Body *body) noexcept
	{
		const auto bytes = body->allocation_bytes();
		body->~Body();
		bodies_.deallocate(body, bytes);
		if (key_.owner == nullptr)
			(void)note_arena();
	}

	/* The arena's bytes changed hands: record them, and say whether they
	 * differ from what was last recorded. The caller holds arena_mutex_. */
	bool note_arena() noexcept
	{
		return ledger_.note_arena(bodies_.bytes());
	}

	/* A body some slot holds now, where none did. The caller holds the shard
	 * lock of its record. */
	void note_held(const Body &body) noexcept
	{
		ledger_.note_held(BodyArena::block_bytes(body.allocation_bytes()));
#if UML_GADGET_SWEEP_STRESS
		const auto period =
			stress_renewal_period.load(std::memory_order_relaxed);
		if (period != 0 &&
		    stress_stores_.fetch_add(1, std::memory_order_relaxed) %
				period == period - 1U)
			ledger_.mark_due();
#endif
	}

	/* The last slot let go of a body. The caller holds the shard lock of its
	 * record. */
	void note_released(const Body &body) noexcept
	{
		ledger_.note_released(
			BodyArena::block_bytes(body.allocation_bytes()),
			renewal_floor());
	}

	/* Mark the space for renewal once the arena holds more bytes no body
	 * uses than bodies do, and at least the floor. */
	void note_idle() noexcept
	{
		ledger_.note_idle(renewal_floor());
	}

	void free_bodies(std::vector<Body *> &bodies) noexcept
	{
		if (bodies.empty())
			return;
		const std::lock_guard arena{arena_mutex_};
		for (auto *body : bodies)
			delete_body(body);
		bodies.clear();
	}

	/* The budget asks the space to shed: every executor attached lets go
	 * of it at its next entry. Called under the budget's lock, which is
	 * only ever taken before attached_mutex_. */
	static void shed(void *owner) noexcept
	{
		try {
			static_cast<Space *>(owner)->request_renewal();
		} catch (...) {
			/* The space is still shedding: the next executor to want
			 * its address space closes it instead of attaching. */
		}
	}

	SpaceKey key_;
	std::array<Shard, shard_count> shards_;
	const arm64::gadgets::BlockLink *empty_link_{};
	/* The code-change sequence links may be activated at, and the one
	 * whose reset has finished. */
	std::atomic<std::uint64_t> gate_;
	std::atomic<std::uint64_t> reset_done_;
	std::mutex reset_mutex_;
	std::mutex targets_mutex_;
	std::unique_ptr<arm64::gadgets::IndirectLinkEntry[]> targets_storage_;
	std::atomic<arm64::gadgets::IndirectLinkEntry *> targets_{};
	gadget_cache::Limbo<Body> limbo_;
	mutable std::mutex arena_mutex_;
	BodyArena bodies_;
	gadget_cache::AttachedExecutors attached_;
	/* For a private address space: the records and sites its last sweep
	 * kept, and the bodies that have left it since (sweep_due()). */
	std::size_t kept_{};
	std::size_t departed_{};
	/* For a shared one: the bytes of its arena, and of the bodies some slot
	 * holds, whether it holds enough freed arena to be renewed, and whether
	 * it has been (renewal_due()). */
	gadget_cache::RenewalLedger ledger_;
#if UML_GADGET_SWEEP_STRESS
	std::atomic<std::uint64_t> stress_stores_{};
#endif
	/* Last, so it is destroyed first: a call of shed() in progress holds
	 * up its destruction, and everything shed() touches is still there. */
	gadget_cache::Account account_;
};

/* The address spaces whose translations executors share, by key. */
using SpaceRegistry = gadget_cache::SpaceRegistry<Space, SpaceKey>;

} // namespace

class GadgetExecutor::ProgramCache final {
	/* Named by member declarations below and defined with the rest of the
	 * private state. */
	struct Generation;

public:
	/* `mode` is the SVCR mode every translation here is built for, and
	 * `announcement` what this cache's executor says about itself; with
	 * `share` false the cache keeps its address spaces to itself. */
	ProgramCache(const BuildOptions &options, std::uint8_t mode,
		const Announcement *announcement, bool share)
		: options_{options}, mode_{mode}, announcement_{announcement},
		  share_{share},
		  build_scratch_{scratch_.data(), scratch_.size()}
	{}

	~ProgramCache()
	{
		for (auto &slab : slabs_) {
			if (!slab)
				continue;
			for (auto &slot : slab->slots) {
				if (slot.body() == nullptr)
					continue;
				space_of(slot).release(slot.body(), false);
				slot.vacate();
			}
		}
		for (auto &[generation, state] : window_.generations) {
			renew_if_due(*state.space);
			SpaceRegistry::instance().detach(state.space,
				announcement_);
		}
	}

	ProgramCache(const ProgramCache &) = delete;
	ProgramCache &operator=(const ProgramCache &) = delete;

#if UML_GADGET_CACHE_CENSUS
	/* Write what this cache holds and how it came to hold it, so
	 * duplication, dormancy and lost work can be counted instead of
	 * estimated. `snapshot` numbers a periodic snapshot; zero is the one
	 * written at teardown.
	 *
	 * A translation is identified by the guest words it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same words but embed pointers to their own link records, so only the
	 * source identifies the work. The parts of its allocation go out with
	 * it because they decide what a body costs. Every address space the
	 * cache holds is written whole, with its identity, so a reader counts a
	 * space that several executors share once, and each translation names
	 * its body so a body several slots hold is counted once.
	 */
	void census(std::FILE *out, std::uint64_t snapshot) const
	{
		/* The executor record counts only what this cache owns: its slabs
		 * and its per-space slot lists. Each address space's bodies and
		 * records follow on an S line named by the space, which every
		 * cache holding it writes and a reader counts once. */
		std::size_t slabs = 0;
		for (const auto &slab : slabs_)
			if (slab)
				slabs += sizeof(Slab);
		std::size_t lists = 0;
		for (const auto &[generation, state] : window_.generations)
			lists += state.slots.capacity() * sizeof(std::uint32_t);
		if (link_memo_)
			lists += sizeof(LinkMemo);
		/* How often the stream lookup was asked, against how often a
		 * store would have had to maintain an index for it. */
		const auto slab_total = static_cast<std::size_t>(
			std::count_if(slabs_.begin(), slabs_.end(),
				[](const auto &slab) { return slab != nullptr; }));
		/* The process's gadget-cache budget, the same on every E line
		 * of a snapshot. */
		const auto book = gadget_cache::ledger_state();
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu slots=%zu slot_bytes=%zu "
			"slab_slots=%zu slab_count=%zu stream_index=0 slabs=%zu "
			"streams=0 arena_empty=0 links=%zu indirect=0 "
			"total=%zu stream_lookups=%llu adoptions=%llu shared=%d "
			"sweeps=%llu swept_links=%llu swept_sites=%llu sweep_ns=%llu "
			"dead_left=%llu lru_tomb=%llu budget=%zu metered=%zu "
			"metered_shedding=%zu metered_peak=%zu reliefs=%llu "
			"shed=%llu",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), slot_count, sizeof(Slot), slab_slots,
			slab_total, slabs, lists, slabs + lists,
			static_cast<unsigned long long>(stream_lookups_),
			static_cast<unsigned long long>(adoptions_),
			share_ ? 1 : 0, static_cast<unsigned long long>(sweeps_),
			static_cast<unsigned long long>(swept_links_),
			static_cast<unsigned long long>(swept_sites_),
			static_cast<unsigned long long>(sweep_ns_),
			static_cast<unsigned long long>(dead_left_),
			static_cast<unsigned long long>(lru_tomb_), book.budget,
			book.total, book.shedding, book.peak_open,
			static_cast<unsigned long long>(book.reliefs),
			static_cast<unsigned long long>(book.shed_accounts));
		census::write_when(out, snapshot);
		history_.write(out);
		std::fputc('\n', out);
		const auto now = census::elapsed_seconds();
		for (const auto &[generation, state] : window_.generations) {
			const auto found = std::find(window_.order.rbegin(),
				window_.order.rend(), generation);
			const bool running =
				generation == window_.current_generation;
			const auto space = state.space->footprint();
			/* Whether the MMU still has the address space: 1 live, 0
			 * destroyed, negative when it cannot say from this thread. */
			const int alive =
				darwin_uml_static_vm_generation_live(generation);
			/* The G line's bytes are this cache's own list of its
			 * slots in the space; the space's are on its S line. */
			std::fprintf(out,
				"G %lu current=%d rank=%td idle_switches=%llu "
				"idle_seconds=%.3f link_records=%zu links=%zu "
				"indirect=0 targets=0 space=%p alive=%d\n",
				generation, running ? 1 : 0,
				found - window_.order.rbegin(),
				static_cast<unsigned long long>(running ? 0 :
					switches_ - state.census_switch),
				running ? 0.0 : now - state.census_seconds,
				state.space->link_records(),
				state.slots.capacity() * sizeof(std::uint32_t),
				static_cast<const void *>(state.space), alive);
			std::fprintf(out,
				"S %p generation=%lu mode=%u attached=%zu "
				"stream_index=%zu streams=%zu arena_empty=%zu "
				"links=%zu indirect=%zu link_records=%zu limbo=%zu "
				"alive=%d charged=%zu shedding=%d\n",
				static_cast<const void *>(state.space), generation,
				unsigned{mode_}, state.space->attached(),
				space.stream_index, space.streams,
				state.space->census_empty_bytes(), space.links,
				space.indirect, state.space->link_records(),
				state.space->limbo_bodies(), alive,
				state.space->charged(),
				state.space->shedding() ? 1 : 0);
		}
		if (census::totals_only())
			return;
		const auto sample = census::word_sample();
		/* The same link targets are sampled in every cache, so the
		 * sample says how many of an address space's records each
		 * cache keeps for itself. A shared space writes its records
		 * from every cache that holds it, which a reader removes by
		 * the space's identity. */
		if (sample != 0)
			for (const auto &[generation, state] :
			     window_.generations)
				state.space->for_each_link(
					[&, generation](const LinkRecord &record) {
						if (census::sampled(
							    record.link.guest_pc,
							    sample))
							std::fprintf(out,
								"L %lu 0x%llx %p\n",
								generation,
								static_cast<
									unsigned long long>(
									record.link
										.guest_pc),
								static_cast<
									const void *>(
									state.space));
					});
		const auto text = census::text_of(reinterpret_cast<const void *>(
			reinterpret_cast<std::uintptr_t>(
				uml_arm64_gadget_link_exit)));
		CensusRecords records;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (slot.body() == nullptr)
					continue;
				const auto &body = *slot.body();
				std::fprintf(out,
					"P %lu 0x%llx %zu %zu %zu 0x%llx %zu %zu %zu "
					"%zu %p\n",
					generation_of(slot),
					static_cast<unsigned long long>(slot.key()),
					std::size_t{body.instruction_count},
					std::size_t{body.word_count},
					std::size_t{body.source_word_count},
					static_cast<unsigned long long>(
						body_digest(body)),
					body_bytes(body), sizeof(Body),
					(body.entry_words() + body.word_count) *
						sizeof(std::uintptr_t),
					std::size_t{body.source_word_count} *
						body.source_unit(),
					static_cast<const void *>(&body));
				if (sample != 0 &&
				    census::sampled(slot.key(), sample))
					census_words(out, slot, text, records);
			}
		}
	}
#endif

	/* How many address spaces currently hold translations here. With one
	 * shared pool this is an outcome of the workload, not a build option. */
	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return window_.generations.size();
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.body() != nullptr)
					++occupied;
		}
		return occupied;
	}

	/* Bind the cache to an address space and answer for whatever changed in
	 * it while it was away. Each address space carries its own mark, so a
	 * switch back to one whose code never changed costs nothing. */
	void prepare(unsigned long generation, std::uint64_t changes)
	{
#if UML_GADGET_CACHE_CENSUS
		/* The address space being left was in use until now. */
		if (generation != window_.current_generation &&
		    window_.current != nullptr) {
			++switches_;
			window_.current->census_switch = switches_;
			window_.current->census_seconds =
				census::elapsed_seconds();
		}
#endif
		window_.current_generation = generation;
		window_.current = nullptr;
		auto &state = current();
		touch(generation);
		if (state.changes == changes)
			return;
		state.space->observe(changes);
		state.changes = changes;
		advance_epoch(state);
	}

	[[nodiscard]] std::optional<BuildResult> find(
		std::uint64_t pc, std::uint64_t changes,
		std::uint64_t mapping_changes, StaticGuestMemory &memory,
		void *execution_context, GadgetCacheDiagnostics &diagnostics)
	{
#if UML_GADGET_CACHE_CENSUS
		if ((history_.lookups() & 4095U) == 0)
			census::snapshot_if_due(schedule_, *this);
#endif
		assert(window_.current == nullptr ||
		       window_.current->changes == changes);
		const auto location = locate(window_.current_generation, pc);
		const auto tag = tag_of(pc);
		auto &slab = slabs_[location.slab];
		if (slab) {
			for (std::size_t way = 0; way < ways; ++way) {
				auto &slot = slab->slots[location.offset + way];
				if (slot.tag != tag)
					continue;
				if (slot.epoch() == window_.current_epoch) {
#if UML_GADGET_CACHE_CENSUS
					history_.hit();
#endif
					return slot.body()->result();
				}
				++diagnostics.validation_attempts;
				if (!validate(slot, changes, mapping_changes,
					    memory, execution_context,
					    diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
					history_.missed();
#endif
					return std::nullopt;
				}
				activate(pc, slot);
#if UML_GADGET_CACHE_CENSUS
				history_.revalidated();
#endif
				return slot.body()->result();
			}
		}
		/* No slot here holds the PC, so ask the address space: another
		 * executor may hold its translation. */
		if (share_) {
			if (auto adopted = adopt(pc, changes, mapping_changes,
				    memory, execution_context, diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
				history_.adopted();
#endif
				return adopted;
			}
		}
#if UML_GADGET_CACHE_CENSUS
		history_.missed();
#endif
		return std::nullopt;
	}

	/* The translation whose stream holds an address.
	 *
	 * A terminal exit inside a linked successor reports the successor's
	 * stream, and the retirement correction needs that successor's program
	 * rather than the entered block's. The arena knows which live body
	 * holds any address, and a stream pointer the engine has just produced
	 * always lies in a live body: links only ever name translations a slot
	 * held when they were linked, and an unpublished body is freed only
	 * after this executor has left translated code. The answer is exact,
	 * with no index to fall out of.
	 */
	[[nodiscard]] std::optional<BuildResult> find_stream(
		const void *stream) const
	{
		if (stream == nullptr || window_.current == nullptr)
			return std::nullopt;
#if UML_GADGET_CACHE_CENSUS
		++stream_lookups_;
#endif
		const auto *body = window_.current->space->owner(stream);
		if (body == nullptr)
			return std::nullopt;
		auto result = body->result();
		if (!program_contains_stream(result.program, stream))
			return std::nullopt;
		return result;
	}

	/* find_stream() with the guest units the translation was validated
	 * against, for an AArch32 exit: a T32 block's instructions are found
	 * in them. */
	struct StreamSource final {
		BuildResult result;
		std::span<const std::byte> source;
	};

	[[nodiscard]] std::optional<StreamSource> find_stream_source(
		const void *stream) const
	{
		if (stream == nullptr || window_.current == nullptr)
			return std::nullopt;
		const auto *body = window_.current->space->owner(stream);
		if (body == nullptr)
			return std::nullopt;
		auto result = body->result();
		if (!program_contains_stream(result.program, stream))
			return std::nullopt;
		return StreamSource{std::move(result), body->source()};
	}

	/* The same lookup for the guest sampler, which runs while this
	 * executor's thread is suspended in gadget code: reads only, and never
	 * waits for a lock another vCPU may hold. */
	[[nodiscard]] bool sample_block(const void *stream,
		diagnostics::SampledBlock &block) const noexcept
	{
		for (const auto &[generation, state] : window_.generations) {
			const Body *body = nullptr;
			try {
				if (!state.space->try_owner(stream, body) ||
				    body == nullptr)
					continue;
			} catch (...) {
				return false;
			}
			const auto *word =
				static_cast<const std::uintptr_t *>(stream);
			if (word < body->link_entry() ||
			    word >= body->words() + body->word_count)
				return false;
			block.guest_address = body->guest_address;
			block.instruction_count = body->instruction_count;
			if (body->isa == 0) {
				block.word_count = std::min<std::uint32_t>(
					body->source_word_count,
					static_cast<std::uint32_t>(
						diagnostics::sampled_block_words));
				std::copy_n(body->source_words(), block.word_count,
					block.words);
				return true;
			}
			/* An AArch32 block's address is its key, and its words
			 * are the guest bytes it was translated from, a T32
			 * block's halfwords in pairs (little-endian words, as
			 * the guest stores them). */
			const auto bytes = std::min<std::size_t>(
				std::size_t{body->source_word_count} *
					body->source_unit(),
				diagnostics::sampled_block_words *
					sizeof(std::uint32_t));
			block.word_count = static_cast<std::uint32_t>(
				(bytes + sizeof(std::uint32_t) - 1) /
				sizeof(std::uint32_t));
			std::fill_n(block.words, block.word_count, 0U);
			std::memcpy(block.words, body->source_bytes(), bytes);
			return true;
		}
		return false;
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(std::uint64_t pc)
	{
		return space().link(pc);
	}

	/* The running address space's target table, or null before its first
	 * indirect miss. It lives and dies with the address space, as the link
	 * records it names do, so it can never name another address space's
	 * code; a record reset by a code change just exits through link_exit. */
	[[nodiscard]] const void *target_tier() const noexcept
	{
		return window_.current != nullptr ?
			window_.current->space->target_tier() : nullptr;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		return space().indirect_link(source_pc);
	}

	/* Where the memo keeps `pc`'s record; activations share it with
	 * indirect misses, since a slot keeps no record of its own. */
	[[nodiscard]] auto &memo_entry(std::uint64_t pc)
	{
		if (!link_memo_)
			link_memo_ = std::make_unique<LinkMemo>();
		return (*link_memo_)[((pc >> 2U) ^ (pc >> 14U) ^
			window_.current_generation) & (link_memo_entries - 1U)];
	}

	[[nodiscard]] bool remembers(const auto &entry,
		std::uint64_t pc) const noexcept
	{
		return entry.link != nullptr && entry.pc == pc &&
			entry.generation == window_.current_generation;
	}

	/* The record for `pc` in the current address space, through this
	 * cache's memo of the records its activations and indirect misses
	 * named. A record never moves while its address space is resident
	 * here, and retiring one clears its entries, as a sweep clears those
	 * naming a record it released, so a hit is always the live record; it
	 * spares the space's lock, which every vCPU's misses would otherwise
	 * meet. */
	[[nodiscard]] const arm64::gadgets::BlockLink *link_record(
		std::uint64_t pc)
	{
		auto &entry = memo_entry(pc);
		if (!remembers(entry, pc))
			entry = {pc, window_.current_generation,
				window_.current->space->link(pc), 0U};
		return entry.link;
	}

	void learn_indirect(arm64::gadgets::IndirectLink &site,
			    std::uint64_t target_pc)
	{
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* Distinct targets seen per site, and misses per site, so the
		 * dump can say how many cells a site would have needed. */
		++site_misses_[&site][target_pc];
#endif
		space().learn_indirect(site, target_pc, link_record(target_pc));
	}

	[[nodiscard]] BuildResult store(
		std::uint64_t pc, const BuildResult &result,
		std::span<const std::uint32_t> guest_words,
		[[maybe_unused]] std::uint64_t changes,
		GadgetCacheDiagnostics &diagnostics)
	{
		/* The words a validating find() will compare against guest
		 * memory: the program's instructions, plus the word a stopped
		 * build read without retiring. */
		std::span<const std::uint32_t> validation;
		const auto validation_words =
			result.program.instruction_count() +
			static_cast<std::size_t>(!result.complete);
		if (validation_words != 0 &&
		    validation_words <= block_instruction_count &&
		    validation_words <= guest_words.size())
			validation = guest_words.first(validation_words);
		return store_validated(pc, result,
			std::as_bytes(validation), changes, diagnostics);
	}

	/* store() for any ISA: `validation` is the guest bytes the program
	 * stands for, in the units the key's ISA reads (aarch32::GuestIsa), so a
	 * T32 builder supplies halfwords and everything else supplies words. */
	[[nodiscard]] BuildResult store_validated(
		std::uint64_t pc, const BuildResult &result,
		std::span<const std::byte> validation,
		[[maybe_unused]] std::uint64_t changes,
		GadgetCacheDiagnostics &diagnostics)
	{
		assert(arm64::aarch32::key_is_canonical(pc));
		/* The running address space is registered before the tag that
		 * names it is made. */
		auto &state = current();
		/* The stored translation is as current as the address space's
		 * code is, which prepare() has been told. */
		assert(state.changes == changes);
		[[maybe_unused]] const auto tag = tag_of(pc);
		/* Nothing is stored under a key with no compact form: such a
		 * key names no fetchable code. */
		assert(tag != unmatchable_tag);
		auto *body = state.space->publish(result, validation,
			arm64::aarch32::key_isa(pc));
#if UML_GADGET_CACHE_CENSUS
		/* A lookup that lost a validation race leaves its translation
		 * resident, and this store makes a second copy beside it. */
		bool restored = false;
		{
			const auto location =
				locate(window_.current_generation, pc);
			if (const auto &slab = slabs_[location.slab])
				for (std::size_t way = 0; way < ways; ++way)
					if (slab->slots[location.offset + way]
						    .tag == tag)
						restored = true;
		}
#endif
		auto &slot = install(pc, body, changes);
#if UML_GADGET_CACHE_CENSUS
		history_.arrive(census_id_, window_.current_generation, pc,
			body_digest(*slot.body()), restored);
#endif
		state.space->reclaim(announcement_);
		/* The build's words are in the body now, so nothing outside what
		 * a sweep reads names a record it made. */
		if (state.space->sweep_due())
			sweep(state, diagnostics);
		renew_if_due(*state.space);
		/* Past the budget, the largest spaces are asked to shed; their
		 * executors let go of them at their next entry. */
		if (gadget_cache::over_budget())
			gadget_cache::relieve();
		return slot.body()->result();
	}

	/* Where a build assembles its stream.
	 *
	 * store() copies the finished words into the body it allocates, so the
	 * scratch is rewound at the start of every build and a translation
	 * costs no allocation until it is stored. Only one build is ever in
	 * flight on an executor.
	 */
	[[nodiscard]] std::pmr::memory_resource *program_storage() noexcept
	{
		build_scratch_.release();
		return &build_scratch_;
	}

	[[nodiscard]] std::uint64_t adoptions() const noexcept
	{
		return adoptions_;
	}

	/* Let go of every address space held here that has been closed, as the
	 * window lets go of the least recently used one, and, with `dead`, of
	 * every one whose generation the MMU has destroyed since the caller last
	 * looked. A destroyed generation never runs again, so nothing it holds
	 * can be looked up again. A closed space's generation leaves the window's
	 * order; a destroyed one's number keeps its place in it as a tombstone,
	 * which holds nothing and which the least-recently-used rule retires when
	 * it reaches it, so the window keeps the live address spaces it would
	 * have kept. Say whether the one last prepared was among those let go of.
	 * The caller runs no translated code of this cache from before this call
	 * until after the next prepare(), which it must therefore make: a retired
	 * space's successor is attached there, at the address space's current
	 * code-change sequence. */
	[[nodiscard]] bool leave_gone(bool dead)
	{
		struct Leaving final {
			unsigned long generation;
			bool tombstone;
		};
		std::array<Leaving, uml_gadget_resident_generations + 1U> leaving{};
		std::size_t count = 0;
		for (const auto &[generation, state] : window_.generations) {
			if (count == leaving.size())
				break;
			if (state.space->closed() || state.space->shedding())
				leaving[count++] = {generation, false};
			else if (dead &&
				 darwin_uml_static_vm_generation_live(generation) == 0)
				leaving[count++] = {generation, true};
		}
		bool left_current = false;
		for (std::size_t index = 0; index < count; ++index) {
			const auto [generation, tombstone] = leaving[index];
			left_current |= generation == window_.current_generation;
#if UML_GADGET_CACHE_CENSUS
			if (tombstone)
				++dead_left_;
#endif
			/* A space the budget asked to shed is closed, for every
			 * other executor holding it to let go of too. */
			if (!tombstone)
				if (const auto held =
					    window_.generations.find(generation);
				    held != window_.generations.end() &&
				    held->second.space->shedding())
					SpaceRegistry::instance().close(
						held->second.space);
			retire(generation, tombstone);
		}
		return left_current;
	}

private:
	static constexpr std::size_t ways = ::uml::cpu::gadget_cache::ways;
	static constexpr std::size_t slot_count = uml_gadget_cache_slots;
	static constexpr std::size_t set_count = slot_count / ways;
	static constexpr std::size_t slab_slots = ::uml::cpu::gadget_cache::slab_slots;
	static constexpr std::size_t slab_count = slot_count / slab_slots;
	/* One way of a set: what a lookup compares and the translation it
	 * names.
	 *
	 * The cache commits a slot per way whether or not it is occupied, so
	 * the tag size multiplies by slot_count: at 262,144 slots every eight
	 * bytes here is two mebibytes per executor. A slot therefore holds
	 * two words, and anything that belongs to one translation belongs
	 * in its Body.
	 */
	struct Slot final {
		/* The translation's key (compact_tag_key) under the id of the
		 * address space it belongs to. A guest virtual address only
		 * means something inside one, so a slot is a match only when
		 * both agree, which is what lets translations for several
		 * address spaces share one cache instead of being wiped at
		 * every switch, and what makes a lookup one comparison. Zero
		 * while the slot is empty: ids start at one, so no occupied
		 * slot's tag is zero and no lookup's is, either.
		 */
		std::uint64_t tag{};
		/* Two fields sharing a word, a pointer below the host's 48-bit
		 * user address limit and sixteen bits above it:
		 *
		 * The translation this slot indexes, held in its address space.
		 * A slot is occupied exactly while it is non-null.
		 *
		 * The epoch of the owner's code (Generation::epoch) when the
		 * translation was last known to match guest memory, zero for
		 * never. It is equal to the owner's current one exactly when no
		 * code change has been seen since.
		 *
		 * The link record a reactivation retargets is not kept here: the
		 * space finds it by the slot's key under the shard lock the
		 * activation takes anyway, which costs one probe of its index per
		 * revalidation instead of a word in every way.
		 */
		std::uint64_t held{};

		static constexpr unsigned epoch_shift = 48;
		static constexpr std::uint64_t body_mask =
			(std::uint64_t{1} << epoch_shift) - 1U;

		[[nodiscard]] Body *body() const noexcept
		{
			return reinterpret_cast<Body *>(
				static_cast<std::uintptr_t>(held & body_mask));
		}
		/* Hold `translation`, known to match guest memory at `value`. */
		void hold(Body *translation, std::uint16_t value) noexcept
		{
			const auto address =
				reinterpret_cast<std::uintptr_t>(translation);
			assert(address != 0U && (address & ~body_mask) == 0U);
			held = address | (std::uint64_t{value} << epoch_shift);
		}
		[[nodiscard]] std::uint16_t epoch() const noexcept
		{
			return static_cast<std::uint16_t>(held >> epoch_shift);
		}
		void set_epoch(std::uint16_t value) noexcept
		{
			held = (held & body_mask) |
				(std::uint64_t{value} << epoch_shift);
		}
		[[nodiscard]] std::uint16_t owner() const noexcept
		{
			return static_cast<std::uint16_t>(
				tag >> gadget_cache::tag_owner_shift);
		}
		[[nodiscard]] std::uint64_t key() const noexcept
		{
			return expand_tag_key(tag);
		}
		/* Empty the slot. The body is the caller's to release. */
		void vacate() noexcept
		{
			tag = 0;
			held = 0;
		}
	};
	static_assert(sizeof(Slot) == 16,
		"cache slots index translations; they do not hold them");

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};
	/* Slabs come from the cache's own heap, which a cache that ends gives back
	 * whole. */
	struct SlabDelete final {
		::uml::memory::DomainHeap *heap{};
		void operator()(Slab *slab) const noexcept
		{
			slab->~Slab();
			heap->deallocate(slab, sizeof(Slab), alignof(Slab));
		}
	};
	using SlabPointer = std::unique_ptr<Slab, SlabDelete>;

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	static constexpr std::size_t link_memo_entries = 4096;
	struct LinkMemoEntry final {
		std::uint64_t pc;
		unsigned long generation;
		const arm64::gadgets::BlockLink *link;
		/* The record's number in its table once an activation
		 * looked it up; zero when only an indirect miss did. */
		std::uint32_t number;
	};
	using LinkMemo = std::array<LinkMemoEntry, link_memo_entries>;

	/* What this cache keeps about one address space it holds: the space,
	 * shared with other executors or not, the code-change mark it last
	 * answered, and the slots it stored into.
	 *
	 * Retiring an address space is then releasing those slots and the
	 * space, rather than filtering it out of structures the others share.
	 * That matters because retirement is not rare on a desktop guest: it
	 * happens on every first sight of a ninth address space. See G167.
	 */
	struct Generation final
		: gadget_cache::GenerationState<Space, unsigned long> {
#if UML_GADGET_CACHE_CENSUS
		/* When the cache last left this address space, in switches and
		 * in seconds. */
		std::uint64_t census_switch{};
		double census_seconds{};
#endif
	};

	using Window = gadget_cache::GenerationWindow<unsigned long, Generation,
		uml_gadget_resident_generations>;

	[[nodiscard]] static Location locate(
		unsigned long generation, std::uint64_t pc) noexcept
	{
		/* Fold the address space into the index so the same guest PC in
		 * two address spaces lands in different sets rather than
		 * competing for the ways of one.
		 */
		const auto block = (pc >> 2) ^
			(static_cast<std::uint64_t>(generation) *
				0x9e3779b97f4a7c15ULL);
		const auto mixed = block ^ (block >> 12) ^ (block >> 24);
		const auto set = static_cast<std::size_t>(mixed) & (set_count - 1);
		const auto first_slot = set * ways;
		return {
			.set = set,
			.slab = first_slot / slab_slots,
			.offset = first_slot % slab_slots,
		};
	}

	void touch(unsigned long generation) { window_.touch(generation); }

	/* Let go of one address space: its resident translations, and the
	 * cache's hold on the space, which goes with the last executor that
	 * held it.
	 *
	 * The slots come from the list the address space kept while it was
	 * storing, so this is proportional to what it owned rather than to the
	 * size of the cache.
	 *
	 * The generation's number leaves the window's order unless `tombstone`
	 * keeps its place there (leave_gone). A number in the order that names
	 * no address space is such a tombstone: retiring it only drops the
	 * place. */
	void retire(unsigned long generation, bool tombstone = false)
	{
		const auto owner = window_.generations.find(generation);
		if (owner == window_.generations.end()) {
			window_.forget_place(generation);
			return;
		}
		const auto id = owner->second.id;
		if (owner->second.slots_complete) {
			for (const auto index : owner->second.slots)
				release(slot_at(index), id);
		} else {
			for (auto &slab : slabs_) {
				if (!slab)
					continue;
				for (auto &slot : slab->slots)
					release(slot, id);
			}
		}
		if (link_memo_)
			for (auto &entry : *link_memo_)
				if (entry.generation == generation)
					entry = {};
		auto *space = owner->second.space;
		/* Everything that can fail comes before anything changes,
		 * so that a failure leaves the address space held and
		 * retirable again. */
		window_.reserve_id();
		/* Letting go of its slots may have left a shared space
		 * mostly freed arena. This cache still holds the space,
		 * which is what makes it safe to close. */
		renew_if_due(*space);
		window_.leave(owner, id);
		SpaceRegistry::instance().detach(space, announcement_);
		if (!tombstone)
			window_.forget_place(generation);
	}

	/* Close a shared address space that holds enough freed arena
	 * (Space::renewal_due), for every executor attached to let go of. Only
	 * one this cache holds, and so is attached to throughout: whoever lets go
	 * of a body closes the space it was in, not just the space it stores
	 * into, since a mostly dead space receives no stores. Called with no lock
	 * of the space held. */
	static void renew_if_due(Space &space)
	{
		if (space.renewal_due())
			SpaceRegistry::instance().close(&space);
	}

	void release(Slot &slot, std::uint16_t owner)
	{
		if (slot.body() == nullptr || slot.owner() != owner)
			return;
#if UML_GADGET_CACHE_CENSUS
		history_.depart(generation_of(slot), slot.key(),
			body_digest(*slot.body()), body_bytes(*slot.body()),
			census::Departure::retired);
#endif
		space_of(slot).release(slot.body(), false);
		slot.vacate();
	}

	/* The address space a slot's translation belongs to. */
	[[nodiscard]] Space &space_of(const Slot &slot) const noexcept
	{
		return *owner_of(slot)->space;
	}

	/* The running address space's translations. */
	[[nodiscard]] Space &space() { return *current().space; }

#if UML_GADGET_CACHE_CENSUS
	/* The census names a translation by a digest of its validation
	 * units. */
	[[nodiscard]] static std::uint64_t body_digest(const Body &body) noexcept
	{
		auto digest = census::digest_seed;
		const auto *bytes = body.source_bytes();
		const auto unit = body.source_unit();
		for (std::size_t index = 0; index < body.source_word_count;
		     ++index) {
			std::uint32_t value = 0;
			std::memcpy(&value, bytes + index * unit, unit);
			digest = census::digest_add(digest, value);
		}
		return digest;
	}

	/* What the arena holds for a body: its allocation, in the arena's
	 * sixteen-byte classes. */
	[[nodiscard]] static std::size_t body_bytes(const Body &body) noexcept
	{
		constexpr std::size_t granule = 16;
		return (std::max(body.allocation_bytes(), granule) + granule - 1) &
			~(granule - 1);
	}

	/* What each record a stream can point at names, for each address
	 * space. */
	using CensusRecords =
		std::unordered_map<unsigned long, census::RecordNames>;

	/* One sampled body's stream, entry word included. */
	void census_words(std::FILE *out, const Slot &slot,
		census::TextRange text, CensusRecords &records) const
	{
		auto [named, inserted] = records.try_emplace(generation_of(slot));
		if (inserted) {
			const auto *owner = owner_of(slot);
			if (owner != nullptr) {
				auto &table = named->second;
				owner->space->for_each_link(
					[&table](const LinkRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.link)] =
							{'l', record.link.guest_pc};
					});
				owner->space->for_each_indirect(
					[&table](const IndirectRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.site)] =
							{'i', record.source_pc};
					});
			}
		}
		const auto &body = *slot.body();
		census::write_words(out, slot.key(),
			{body.link_entry(), body.words() + body.word_count},
			text, named->second);
	}
#endif

	[[nodiscard]] Slot &slot_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / slab_slots];
		return slab->slots[index % slab_slots];
	}

	/* The address space translation is currently running in, bound to its
	 * space -- made, or shared with the other executors running it -- on
	 * first sight, when the least recently current one beyond the resident
	 * bound is retired.
	 *
	 * prepare() sets this on every switch, but its caller may skip the call
	 * when nothing changed, and on the very first entry there is nothing to
	 * compare against -- so the binding is completed here rather than
	 * assumed. */
	[[nodiscard]] Generation &current()
	{
		if (window_.current != nullptr)
			return *window_.current;
		auto [entry, inserted] = window_.generations.try_emplace(
			window_.current_generation);
		window_.current = &entry->second;
		if (inserted) {
			auto &state = entry->second;
			window_.enroll(state, window_.current_generation);
			state.space = SpaceRegistry::instance().attach(
				SpaceKey{
					.generation = window_.current_generation,
					.mode = mode_,
					.owner = share_ ? nullptr : this,
					.options = options_,
				},
				state.changes, announcement_);
			touch(window_.current_generation);
			window_.trim([this](unsigned long generation) {
#if UML_GADGET_CACHE_CENSUS
				if (!window_.generations.contains(generation))
					++lru_tomb_;
#endif
				retire(generation);
			});
		}
		window_.bind(*window_.current);
		return *window_.current;
	}

	/* The tag a lookup in the running address space compares. */
	[[nodiscard]] std::uint64_t tag_of(std::uint64_t key) const noexcept
	{
		const auto compact = compact_tag_key(key);
		return compact == unmatchable_tag ? unmatchable_tag :
			compact | window_.current_owner;
	}

	[[nodiscard]] Generation *owner_of(const Slot &slot) const noexcept
	{
		return window_.owners[slot.owner()];
	}

	[[nodiscard]] unsigned long generation_of(
		const Slot &slot) const noexcept
	{
		return window_.owners[slot.owner()]->value;
	}

	/* A code change was seen in the running address space: every slot's
	 * validation is out of date. When the epoch would pass what a slot can
	 * hold, the slots that carry one are cleared first. */
	void advance_epoch(Generation &state) noexcept
	{
		const auto forget_epochs = [this](Generation &wrapped) noexcept {
			const auto id = wrapped.id;
			const auto forget = [id](Slot &slot) noexcept {
				if (slot.body() != nullptr && slot.owner() == id)
					slot.set_epoch(0);
			};
			if (wrapped.slots_complete) {
				for (const auto index : wrapped.slots)
					forget(slot_at(index));
			} else {
				for (auto &slab : slabs_)
					if (slab)
						for (auto &slot : slab->slots)
							forget(slot);
			}
		};
		window_.advance_epoch(state, forget_epochs);
	}

	/* Put a held body in the set for `pc`, letting go of whatever the way
	 * it takes held, and link it. */
	Slot &install(std::uint64_t pc, Body *body,
		[[maybe_unused]] std::uint64_t changes)
	{
		assert(current().changes == changes);
		const auto location = locate(window_.current_generation, pc);
		auto &slab = slabs_[location.slab];
		if (!slab)
			slab = SlabPointer{new (slab_heap_.allocate(sizeof(Slab),
				alignof(Slab))) Slab{}, SlabDelete{&slab_heap_}};
		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.body() == nullptr) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>((selected + 1) & (ways - 1));

		auto &slot = slab->slots[location.offset + selected];
		/* Record the slot against the address space taking it, so its
		 * retirement walks what it owns instead of the whole cache. */
		auto &state = current();
		if (state.slots_complete) {
			if (state.slots.size() < slot_count) {
				state.slots.push_back(static_cast<std::uint32_t>(
					location.slab * slab_slots +
					location.offset + selected));
			} else {
				state.slots_complete = false;
				state.slots.clear();
				state.slots.shrink_to_fit();
			}
		}
		/* Whatever occupied the slot lets go of its body, whose address
		 * space may not be the running one. */
		if (slot.body() != nullptr) {
#if UML_GADGET_CACHE_CENSUS
			history_.depart(generation_of(slot), slot.key(),
				body_digest(*slot.body()), body_bytes(*slot.body()),
				census::Departure::evicted);
#endif
			auto &evicted = space_of(slot);
			evicted.release(slot.body(), false);
			slot.vacate();
			renew_if_due(evicted);
		}
		/* The activation finds the record in the running address space,
		 * which may not be the one that held the slot until now. */
		slot.tag = tag_of(pc);
		slot.hold(body, window_.current_epoch);
		activate(pc, slot);
		return slot;
	}

	/* Take the translation another executor of the address space holds for
	 * `pc` instead of building it: validated against guest memory exactly
	 * as a stale slot is, then held and linked like a fresh store. */
	[[nodiscard]] std::optional<BuildResult> adopt(std::uint64_t pc,
		std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		auto &space = this->space();
		auto *body = space.hold_indexed(pc);
		if (body == nullptr)
			return std::nullopt;
		const auto verdict = validate_body(*body, pc, changes,
			mapping_changes, memory, execution_context);
		if (verdict != Validation::valid) {
			space.release(body, verdict == Validation::stale);
			return std::nullopt;
		}
		++adoptions_;
		++diagnostics.adoptions;
		auto &slot = install(pc, body, changes);
		space.reclaim(announcement_);
		renew_if_due(space);
		return slot.body()->result();
	}

	void discard(Slot &slot)
	{
		if (slot.body() != nullptr) {
#if UML_GADGET_CACHE_CENSUS
			history_.depart(generation_of(slot), slot.key(),
				body_digest(*slot.body()), body_bytes(*slot.body()),
				census::Departure::discarded);
#endif
			auto &space = space_of(slot);
			space.release(slot.body(), true);
			slot.vacate();
			space.reclaim(announcement_);
			renew_if_due(space);
		}
	}

	enum class Validation : std::uint8_t { valid, stale, race };

	/* Compare a body's guest units with guest memory: words, or for T32
	 * halfwords, read where the key says they lie, and only while the
	 * address space's sequences stay where they were. */
	[[nodiscard]] Validation validate_body(const Body &body,
		std::uint64_t pc, std::uint64_t changes,
		std::uint64_t mapping_changes, StaticGuestMemory &memory,
		void *execution_context, GadgetCacheDiagnostics *diagnostics =
			nullptr)
	{
		const auto units = std::size_t{body.source_word_count};
		const auto limit = body.isa == 0 ? block_instruction_count :
			maximum_t32_validation_units;
		if (units == 0 || units > limit) {
			if (diagnostics != nullptr)
				++diagnostics->validation_mismatches;
			return Validation::stale;
		}

		/* The saved units are the block's instructions and, for a build
		 * that stopped, the one it read without retiring. */
		std::array<std::uint32_t, block_instruction_count + 1> fetched;
		const auto outcome = arm64::aarch32::validate_guest_units(
			pc, units, body.source_bytes(),
			std::as_writable_bytes(std::span{fetched}),
			guest_page_size,
			[&memory](std::uint64_t address,
				  std::span<std::byte> bytes) {
				return memory.fetch(address, bytes);
			},
			false);
		if (outcome == arm64::aarch32::GuestValidation::fault) {
			if (diagnostics != nullptr)
				++diagnostics->validation_faults;
			return Validation::stale;
		}
		if (outcome == arm64::aarch32::GuestValidation::mismatch) {
			if (diagnostics != nullptr)
				++diagnostics->validation_mismatches;
			return Validation::stale;
		}

		const auto current =
			darwin_uml_static_vm_execution_context_sequences(
				execution_context);
		if (current.execution_interrupt != 0 ||
		    current.mapping_changes != mapping_changes ||
		    current.code_changes != changes) {
			if (diagnostics != nullptr)
				++diagnostics->validation_sequence_races;
			return Validation::race;
		}
		return Validation::valid;
	}

	[[nodiscard]] bool validate(
		Slot &slot, std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		switch (validate_body(*slot.body(), slot.key(), changes,
			mapping_changes, memory, execution_context,
			&diagnostics)) {
		case Validation::stale:
			discard(slot);
			return false;
		case Validation::race:
			return false;
		case Validation::valid:
			break;
		}
		slot.set_epoch(window_.current_epoch);
		++diagnostics.validation_hits;
		return true;
	}

	void activate(std::uint64_t pc, const Slot &slot)
	{
		const auto &body = *slot.body();
		if (!body.complete || body.instruction_count == 0)
			return;
		auto &entry = memo_entry(pc);
		const auto known = remembers(entry, pc) && entry.number != 0;
		const auto [number, link] = space().activate(pc,
			known ? entry.number : 0U, body, current().changes);
		if (!known)
			entry = {pc, window_.current_generation, link, number};
	}

	/* Release the link records and indirect sites of the running address
	 * space, which this cache keeps to itself, that nothing it holds names
	 * any more.
	 *
	 * A record is made for every PC a translation links to and a site for
	 * every indirect branch, and nothing else releases one, so they grew
	 * with all the code the address space ever ran instead of with what the
	 * cache holds (G226). What can name one is known: a resident
	 * translation's stream words name its successors' records and its
	 * sites, a resident block keeps its own record because only an
	 * activation arms it and a hit does not activate, and empty cells name
	 * the empty record. Every word of every stream is tried, operands too;
	 * one that happens to equal a record's address only keeps that record.
	 * The space empties the site cells and target-table entries that named
	 * what went, this cache forgets its memo of them, and the CPU's return
	 * ring may name one too, so an executor that sees `link_sweeps` move
	 * across a store empties the ring.
	 *
	 * Only a store calls this, after the build's words are in their body. */
	void sweep(Generation &state, GadgetCacheDiagnostics &diagnostics) noexcept
	{
#if UML_GADGET_CACHE_CENSUS
		const auto started = std::chrono::steady_clock::now();
#endif
		try {
			sweep_records(state, diagnostics);
		} catch (const std::bad_alloc &) {
			/* A sweep only returns memory; without the memory to run
			 * one, the records stay. */
		}
		state.space->swept();
		++diagnostics.link_sweeps;
#if UML_GADGET_CACHE_CENSUS
		++sweeps_;
		sweep_ns_ += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count());
#endif
	}

	/* sweep(), with everything it allocates allocated before anything is
	 * released. */
	void sweep_records(Generation &state, GadgetCacheDiagnostics &diagnostics)
	{
		auto &space = *state.space;
		auto [links, sites] = space.marks();
		std::vector<std::uint64_t> seen((slot_count + 63) / 64);
		std::vector<std::uint32_t> held;
		const auto visit = [&](std::uint32_t index) {
			const auto &slot = slot_at(index);
			if (slot.body() == nullptr || slot.owner() != state.id ||
			    (seen[index / 64] >> (index % 64) & 1U) != 0)
				return;
			seen[index / 64] |= std::uint64_t{1} << (index % 64);
			held.push_back(index);
			if (const auto *own = space.find_link(slot.key()))
				links.mark(own);
			const auto &body = *slot.body();
			const auto *end = body.words() + body.word_count;
			for (const auto *word = body.link_entry(); word != end;
			     ++word) {
				links.mark(*word - offsetof(LinkRecord, link));
				sites.mark(*word - offsetof(IndirectRecord, site));
			}
		};
		if (state.slots_complete) {
			for (const auto index : state.slots)
				visit(index);
		} else {
			for (std::size_t slab = 0; slab < slab_count; ++slab)
				if (slabs_[slab])
					for (std::size_t offset = 0;
					     offset < slab_slots; ++offset)
						visit(static_cast<std::uint32_t>(
							slab * slab_slots + offset));
		}
		held.shrink_to_fit();
		/* The memo of the records activations and indirect misses
		 * named may hold one about to go. */
		if (link_memo_)
			for (auto &entry : *link_memo_)
				if (entry.generation ==
					    window_.current_generation &&
				    entry.link != nullptr &&
				    !links.marked(Space::record_of(entry.link)))
					entry = {};
		state.slots = std::move(held);
		state.slots_complete = true;
		const auto released = space.sweep(links, sites);
		diagnostics.released_links += released.links;
		diagnostics.released_sites += released.sites;
#if UML_GADGET_CACHE_CENSUS
		swept_links_ += released.links;
		swept_sites_ += released.sites;
#endif
	}

#if UML_GADGET_EXECUTION_HISTOGRAM
public:
	std::unordered_map<const arm64::gadgets::IndirectLink *,
		std::unordered_map<std::uint64_t, std::uint64_t>> site_misses_;
	[[nodiscard]] std::array<std::uint64_t, 8> miss_distribution() const
	{
		/* misses at sites with 1, 2, 3, 4, 5-8, 9-16, 17-64, >64 targets */
		std::array<std::uint64_t, 8> out{};
		for (const auto &[site, targets] : site_misses_) {
			std::uint64_t misses = 0;
			for (const auto &[pc, count] : targets)
				misses += count;
			const auto n = targets.size();
			const std::size_t bucket = n <= 4 ? n - 1 : n <= 8 ? 4 :
				n <= 16 ? 5 : n <= 64 ? 6 : 7;
			out[bucket] += misses;
		}
		return out;
	}
private:
#endif
	BuildOptions options_;
	std::uint8_t mode_{};
	const Announcement *announcement_{};
	bool share_{};
	std::uint64_t adoptions_{};
	/* The records indirect misses named, made at the first one. */
	std::unique_ptr<LinkMemo> link_memo_;
	/* Large enough that an ordinary build never reaches the upstream: a
	 * stream that doubles its way to 272 words consumes 510 words of
	 * scratch on the way. */
	std::array<std::byte, 32U << 10> scratch_;
	std::pmr::monotonic_buffer_resource build_scratch_;
	/* At most uml_gadget_resident_generations + 1 entries, so finding one is
	 * cheap; the state each holds is what makes retiring it proportional.
	 * The running one stays valid across insertion; only retiring this very
	 * address space could invalidate it, and retirement only ever takes the
	 * least recently current one. */
	Window window_;
#if UML_GADGET_CACHE_CENSUS
	mutable std::uint64_t stream_lookups_{};
	census::History history_;
	census::Schedule schedule_;
	unsigned census_id_{census::next_cache_id()};
	/* Address-space switches, the clock dormancy is counted in. */
	std::uint64_t switches_{};
	/* Link sweeps, what they released, and the time they took. */
	std::uint64_t sweeps_{};
	std::uint64_t swept_links_{};
	std::uint64_t swept_sites_{};
	std::uint64_t sweep_ns_{};
	/* Address spaces let go of because their generation was destroyed, and
	 * the tombstones of those the window's order retired. */
	std::uint64_t dead_left_{};
	std::uint64_t lru_tomb_{};
#endif
	::uml::memory::DomainHeap slab_heap_;
	std::array<SlabPointer, slab_count> slabs_;
	std::array<std::uint8_t, set_count> next_replacement_{};
};

GadgetExecutor::GadgetExecutor(arm64::gadgets::BuildOptions build_options)
	: build_options_{build_options},
	  sampling_{diagnostics::guest_sampling_register(*this)}
{
	tlb_.unaligned_atomics =
		build_options_.host_capabilities.has(ArmFeature::lse2);
	/* Executors running one address space share its translations when
	 * sharing is enabled, the host retargets a link pair in one access, and
	 * a reclaimer can see whether this executor is running translated
	 * code. */
	announcement_ = acquire_announcement();
	share_translations_ = announcement_ != nullptr &&
		build_options_.host_capabilities.has(ArmFeature::lse2) &&
		shared_bodies_enabled();
	programs_ = std::make_unique<ProgramCache>(build_options_, 0,
		announcement_, share_translations_);
	retirements_seen_ = darwin_uml_static_vm_generation_retirements();
}

GadgetExecutor::~GadgetExecutor()
{
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_unregister(*sampling_);
#if UML_GADGET_EXECUTION_HISTOGRAM
	diagnostics::execution_histogram_forget_executor(&cache_diagnostics_);
#endif
#if UML_GADGET_CACHE_CENSUS
	/* Every executor of the run reports its own caches, and the union
	 * across them is the measurement. */
	census::append([this](std::FILE *out) {
		programs_->census(out, 0);
		for (const auto &mode : sme_caches_)
			if (mode.programs)
				mode.programs->census(out, 0);
	});
#endif
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	tlb_.execution_context = nullptr;
	/* The caches let go of their address spaces before the executor stops
	 * being one a reclaimer waits for. */
	programs_.reset();
	for (auto &mode : sme_caches_)
		mode.programs.reset();
	release_announcement(announcement_);
}

namespace {

/* A cache generation and code-change sequence no run has: they make the next
 * prepare_cache() prepare. */
constexpr unsigned long no_generation = ~0UL;
constexpr std::uint64_t unprepared_changes = ~std::uint64_t{};

} // namespace

/* Let go of the address spaces another executor closed because they held
 * mostly freed arena (Space::renewal_due), and, when the MMU has destroyed a
 * generation since this executor last looked, of those whose generation it
 * destroyed, which no vCPU can enter again. This executor runs no translated
 * code here, and its return ring empties at the first select_cpu() of the
 * run that follows, which is what window retirement relies on too. A cache
 * that lost the address space it last prepared must prepare again, so that
 * the successor is attached at the code-change sequence the guest is at
 * rather than the zero a fresh binding starts from. */
void GadgetExecutor::leave_gone_spaces(std::uint64_t retirements) noexcept
{
	const bool dead = retirements != retirements_seen_;
	const bool closed = announcement_ != nullptr &&
		announcement_->renew.exchange(false, std::memory_order_acquire);
	if (!dead && !closed)
		return;
	retirements_seen_ = retirements;
	try {
		if (programs_->leave_gone(dead)) {
			cache_generation_ = no_generation;
			cache_changes_ = unprepared_changes;
		}
		for (auto &mode : sme_caches_)
			if (mode.programs && mode.programs->leave_gone(dead)) {
				mode.generation = no_generation;
				mode.changes = unprepared_changes;
			}
	} catch (...) {
		/* Out of memory part way, so some may still be held: ask again at
		 * the next entry, and prepare afresh meanwhile, since the one last
		 * prepared may be among those let go of. */
		if (announcement_ != nullptr)
			announcement_->renew.store(true, std::memory_order_relaxed);
		retirements_seen_ = retirements - 1U;
		cache_generation_ = no_generation;
		cache_changes_ = unprepared_changes;
		for (auto &mode : sme_caches_) {
			mode.generation = no_generation;
			mode.changes = unprepared_changes;
		}
	}
}

bool GadgetExecutor::begin_execution(unsigned long generation,
				     std::uint64_t guest_pc)
{
	preempted_ = false;
	/* Two loads decide: whether the MMU destroyed a generation since this
	 * executor last looked, and whether another executor closed a space it
	 * holds. */
	const auto retirements = darwin_uml_static_vm_generation_retirements();
	if (retirements != retirements_seen_ ||
	    (announcement_ != nullptr &&
	     announcement_->renew.load(std::memory_order_relaxed)))
		leave_gone_spaces(retirements);
	auto *context = execution_context(generation);
	if (context == nullptr ||
	    darwin_uml_static_vm_execution_context_begin(context, guest_pc) != 0)
		return false;
	if (announcement_ != nullptr)
		announce_entry(*announcement_);
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_enter(*sampling_);
	return true;
}

void GadgetExecutor::end_execution() noexcept
{
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_leave(*sampling_);
	if (current_cpu_ != nullptr) {
		/* A miss is a pointer into this executor's cache, not architectural
		 * state. Learning the target can allocate after the guest branch has
		 * retired; discard the marker even when that postamble throws. */
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	current_cpu_ = nullptr;
	if (execution_context_ != nullptr)
		(void)darwin_uml_static_vm_execution_context_end(execution_context_);
	if (announcement_ != nullptr)
		announce_exit(*announcement_);
}

void GadgetExecutor::select_cpu(CpuState &cpu) noexcept
{
	if (current_cpu_ == &cpu)
		return;
	cpu.gadget_return_stack = {};
	cpu.gadget_return_top = 0;
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	/* The marker is executor-private scratch. Never inherit it across CPU or
	 * execution reuse; end_execution() resets current_cpu_ at every exit. */
	cpu.gadget_indirect_miss = nullptr;
	current_cpu_ = &cpu;
	if (sampling_ != nullptr)
		diagnostics::guest_sampling_thread(*sampling_, &cpu.tls);
}

void *GadgetExecutor::execution_context(unsigned long generation)
{
	if (execution_context_ != nullptr && execution_generation_ == generation)
		return execution_context_;
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	execution_context_ =
		darwin_uml_static_vm_execution_context_create(generation);
	execution_generation_ = execution_context_ != nullptr ? generation : 0;
	tlb_.execution_context = execution_context_;
	const auto *sequence = execution_context_ != nullptr ?
		darwin_uml_static_vm_execution_context_code_sequence(
			execution_context_) : nullptr;
	code_sequence_ = sequence != nullptr ? sequence : &arm64::no_code_changes;
	return execution_context_;
}

void GadgetExecutor::prepare_cache(
	unsigned long generation, std::uint64_t changes)
{
	if (cache_generation_ == generation && cache_changes_ == changes)
		return;
	/* Switching address space does not empty the cache: slots, links and
	 * indirect sites all carry the address space they describe, so the
	 * translations of the one being left stay resident for its return. */
	programs_->prepare(generation, changes);
	cache_generation_ = generation;
	cache_changes_ = changes;
}

bool GadgetExecutor::sample_block(const void *stream,
	diagnostics::SampledBlock &block) const noexcept
{
	if (programs_->sample_block(stream, block))
		return true;
	for (const auto &mode : sme_caches_)
		if (mode.programs && mode.programs->sample_block(stream, block))
			return true;
	return false;
}

std::optional<BuildResult> GadgetExecutor::find(
	std::uint64_t pc, std::uint64_t changes, StaticGuestMemory &memory)
{
	return programs_->find(
		pc, changes, mapping_changes_, memory, execution_context_,
		cache_diagnostics_);
}

namespace {

/* A store whose link sweep released records may have released one the return
 * ring names. The ring only predicts, so it starts again empty. */
void forget_returns(arm64::CpuState &cpu) noexcept
{
	cpu.gadget_return_stack = {};
	cpu.gadget_return_top = 0;
}

#if UML_GADGET_EXECUTION_HISTOGRAM
/* Why translated execution returned to the executor, for the execution
 * histogram: a guest-visible event, the chain limit, an indirect miss, or an
 * unlinked exit. */
template<typename Programs>
void note_exit_histogram(GadgetCacheDiagnostics &d, Programs &programs,
	::uml::cpu::EngineEvent event, const arm64::CpuState &cpu,
	std::size_t executed)
{
	if (event == ::uml::cpu::EngineEvent::system_call)
		++d.exits_syscall;
	else if (event == ::uml::cpu::EngineEvent::memory_fault)
		++d.exits_fault;
	else if (event == ::uml::cpu::EngineEvent::interrupt)
		++d.exits_interrupt;
	else if (event != ::uml::cpu::EngineEvent::continued)
		++d.exits_other;
	else if (cpu.gadget_indirect_miss != nullptr) {
		++d.exits_indirect_miss;
		if (static_cast<const arm64::gadgets::IndirectLink *>(
			    cpu.gadget_indirect_miss)->kind != 0)
			++d.exits_indirect_miss_return;
		if ((d.exits_indirect_miss & 4095) == 0)
			d.miss_by_site_targets = programs.miss_distribution();
	}
	else if (executed >= 4096)
		++d.exits_chain_limit;
	else
		++d.exits_unlinked;
	diagnostics::execution_histogram_note_executor(&d);
}
#endif

} // namespace

/* Produce and store a translation the cache did not hold. run_block
 * counts it: with the count in here, run_block's per-dispatch cache lookup
 * compiles differently. */
BuildResult GadgetExecutor::translate(
	std::uint64_t pc, std::span<const std::uint32_t> window)
{
	const auto decoded = arm64::gadgets::Builder{}.build(
		pc, window,
		[this](std::uint64_t target) {
			return programs_->link(target);
		},
		programs_->program_storage(),
		[this](std::uint64_t source_pc) {
			return programs_->indirect_link(source_pc);
		},
		build_options_);
	const auto sweeps = cache_diagnostics_.link_sweeps;
	auto stored = programs_->store(pc, decoded, window, cache_changes_,
		cache_diagnostics_);
	if (cache_diagnostics_.link_sweeps != sweeps && current_cpu_ != nullptr)
		forget_returns(*current_cpu_);
	return stored;
}

[[gnu::always_inline]] inline ::uml::cpu::EngineEvent
GadgetExecutor::finish_block(ProgramCache &programs, const BuildResult &built,
	::uml::cpu::EngineEvent event, arm64::CpuState &cpu,
	std::size_t &executed)
{
	const bool fpcr_enable_exit =
		event == ::uml::cpu::EngineEvent::continued &&
		(cpu.effective_fpcr() &
		 arm64::detail::fpcr_exception_enable_mask) != 0U;
	const bool count_exit =
		terminal_does_not_retire(event) || fpcr_enable_exit;
	if (count_exit) {
		const auto *current_program = &built.program;
		const auto *exit_stream = cpu.gadget_indirect_miss;
		std::optional<BuildResult> exit_block;
		if (!program_contains_stream(*current_program, exit_stream)) {
			exit_block = programs.find_stream(exit_stream);
			if (exit_block) {
				current_program = &exit_block->program;
				arm64::gadgets::resolve_fault_position(
					*current_program, cpu.pc);
			}
		}
		if (fpcr_enable_exit)
			correct_fpcr_enable_instruction_count(
				*current_program, event, cpu, executed);
		else
			correct_terminal_instruction_count(
				*current_program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
	}
#if UML_GADGET_EXECUTION_HISTOGRAM
	note_exit_histogram(cache_diagnostics_, programs, event, cpu, executed);
#endif
	/* A stream pointer left by a non-retiring or FPCR-enable exit was
	 * consumed and cleared above, so what remains is a cache cell. */
	if (cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs.learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	tlb_.fault_alignment = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	std::array<std::uint32_t, block_instruction_count> instructions;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	/* What the translations this run reaches are validated at, below: an
	 * ISB leaves when the address space's code has changed since. */
	cpu.code_sequence = code_sequence_;
	cpu.code_sequence_seen = sequences.code_changes;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	tlb_.tagged_addresses = cpu.tbi_enabled;
	if (cpu.svcr != 0U) [[unlikely]]
		return run_sme_block(cpu, memory, generation,
			sequences.code_changes, executed);
	/* CpuState::effective_fpcr(), open-coded: through the member this
	 * block-entry test lowers to a word load and mask instead of a byte
	 * test, and run_block inlines more around it. */
	const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
	if (single_step_ || (guest_fpcr &
	     arm64::detail::fpcr_exception_enable_mask) != 0U)
		return run_trap_enabled_instruction(cpu, memory, executed);
	if (cache_generation_ != generation) {
		/* Return records name link state of the address space being
		 * left; the ring must not survive into another. */
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	prepare_cache(generation, sequences.code_changes);
	auto selected = find(cpu.pc, sequences.code_changes, memory);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			return fetch_fault(tlb_, memory);
		}
		const auto window = std::span{instructions}.first(count);
		++cache_diagnostics_.translations;
		selected.emplace(translate(cpu.pc, window));
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	/* Published on every entry: the CpuState may last have run another
	 * address space, whose table this cache may since have retired. */
	cpu.gadget_target_tier = programs_->target_tier();
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	return finish_block(*programs_, built, event, cpu, executed);
}

/* The SME modes' run_block. Its cache, links and target table are the
 * mode's own, so everything the program can reach was translated for the
 * mode it runs in; a guest SVCR write ends its block, and the next run looks
 * its successor up in the new mode. */
[[gnu::noinline]] ::uml::cpu::EngineEvent GadgetExecutor::run_sme_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory,
	unsigned long generation, std::uint64_t code_changes,
	std::size_t &executed)
{
	const auto mode = static_cast<std::uint8_t>(cpu.svcr & arm64::svcr_mask);
	/* SVCR leaves 0 only through an SME instruction or an SME task the
	 * kernel loaded, and either needs the profile and a register file. */
	if (mode != cpu.svcr || cpu.sme_registers == nullptr ||
	    !arm64::implements_sme(build_options_.host_capabilities))
		return ::uml::cpu::EngineEvent::fatal;
	/* Floating-point exceptions never trap in streaming mode. */
	const bool streaming = (mode & arm64::svcr_sm) != 0U;
	if (single_step_ || (!streaming && (cpu.effective_fpcr() &
	     arm64::detail::fpcr_exception_enable_mask) != 0U))
		return run_trap_enabled_instruction(cpu, memory, executed);
	/* A streaming program runs natively in streaming mode; an injected
	 * profile without the hardware cannot run one. */
	if (streaming && !build_options_.host_capabilities.sme_hardware)
		return ::uml::cpu::EngineEvent::fatal;
	auto &state = sme_caches_[mode - 1U];
	if (!state.programs)
		state.programs = std::make_unique<ProgramCache>(build_options_,
			mode, announcement_, share_translations_);
	auto &programs = *state.programs;
	if (state.generation != generation) {
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	if (state.generation != generation || state.changes != code_changes) {
		programs.prepare(generation, code_changes);
		state.generation = generation;
		state.changes = code_changes;
	}
	auto selected = programs.find(cpu.pc, code_changes, mapping_changes_,
		memory, execution_context_, cache_diagnostics_);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		std::array<std::uint32_t, block_instruction_count> instructions;
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			return fetch_fault(tlb_, memory);
		}
		const auto window = std::span{instructions}.first(count);
		++cache_diagnostics_.translations;
		auto options = build_options_;
		options.svcr = mode;
		const auto decoded = arm64::gadgets::Builder{}.build(
			cpu.pc, window,
			[&programs](std::uint64_t target) {
				return programs.link(target);
			},
			programs.program_storage(),
			[&programs](std::uint64_t source_pc) {
				return programs.indirect_link(source_pc);
			},
			options);
		const auto sweeps = cache_diagnostics_.link_sweeps;
		selected.emplace(programs.store(cpu.pc, decoded, window,
			code_changes, cache_diagnostics_));
		if (cache_diagnostics_.link_sweeps != sweeps)
			forget_returns(cpu);
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	cpu.access_sme_state();
	cpu.gadget_target_tier = programs.target_tier();
	if (!streaming) {
		const auto event =
			arm64::gadgets::execute_preserving_native_fp_environment(
				built.program, cpu, tlb_, executed);
		return finish_block(programs, built, event, cpu, executed);
	}
	/* Streaming programs carry no FP link entry; the FP state they can
	 * reach is loaded here, once, for every program they link to. XNU
	 * resets FPSR on every kernel entry while a thread is in streaming
	 * mode, so the guest's FPSR must not be in the host register: leave
	 * the native FP environment first, as the SVCR write that entered
	 * streaming mode already did. */
	arm64::gadgets::restore_native_fp_environment(cpu);
	cpu.access_fp_state();
	const auto event =
		arm64::gadgets::execute_streaming_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed,
			arm64::sme_profile(build_options_.host_capabilities) ==
				arm64::SmeProfile::sme2);
	return finish_block(programs, built, event, cpu, executed);
}

/* ------------------------------------------------------------ AArch32 */

namespace {

/* Restores what an AArch32 entry changes in the TLB that an A64 entry does
 * not set again: address tagging is set at every A64 entry, but whether an
 * unaligned exclusive faults is configured once. AArch32 has no LSE2 form of
 * LDREX, so every misaligned exclusive faults. */
class A32TlbScope final {
public:
	A32TlbScope(::uml::memory::Tlb &tlb) noexcept
		: tlb_{tlb}, unaligned_atomics_{tlb.unaligned_atomics}
	{
		tlb_.tagged_addresses = false;
		tlb_.unaligned_atomics = false;
	}
	~A32TlbScope() { tlb_.unaligned_atomics = unaligned_atomics_; }
	A32TlbScope(const A32TlbScope &) = delete;
	A32TlbScope &operator=(const A32TlbScope &) = delete;

private:
	::uml::memory::Tlb &tlb_;
	bool unaligned_atomics_;
};

} // namespace

::uml::cpu::EngineEvent GadgetExecutor::run_a32_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	tlb_.fault_alignment = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	/* What the translations this run reaches are validated at, below: an
	 * ISB leaves when the address space's code has changed since. */
	cpu.code_sequence = code_sequence_;
	cpu.code_sequence_seen = sequences.code_changes;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	/* An AArch32 task never has a streaming mode, and the profile has no
	 * trap-enabled floating point. */
	if (cpu.svcr != 0U)
		return ::uml::cpu::EngineEvent::fatal;
	const A32TlbScope tlb_scope{tlb_};
	/* A single step, and an entry in the middle of an IT block, run one
	 * instruction uncached until ITSTATE is zero again. */
	if (single_step_ || cpu.a32_itstate != 0U)
		return run_a32_instruction(cpu, memory, executed);
	return run_a32_translation(cpu, memory, generation,
		sequences.code_changes, executed);
}

::uml::cpu::EngineEvent GadgetExecutor::run_a32_translation(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::uint64_t code_changes, std::size_t &executed)
{
	if (cache_generation_ != generation) {
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	prepare_cache(generation, code_changes);
	const auto key = cpu.pc;
	auto selected = find(key, code_changes, memory);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
	if (!selected) {
		const auto isa = arm64::aarch32::key_isa(key);
		const auto unit = arm64::aarch32::isa_unit_bytes(isa);
		std::array<std::uint32_t, block_instruction_count> window;
		const auto bytes = std::as_writable_bytes(std::span{window});
		const auto address = arm64::aarch32::key_pc(key);
		const auto count = fetch_units(memory, address, unit, bytes);
		if (count == 0) {
			return fetch_fault(tlb_, memory);
		}
		++cache_diagnostics_.translations;
		const auto output = arm64::gadgets::a32::build(
			{.key = key, .code = bytes.first(count * unit)},
			[this](std::uint64_t target) {
				return programs_->link(target);
			},
			programs_->program_storage(),
			[this](std::uint64_t source_key) {
				return programs_->indirect_link(source_key);
			},
			build_options_,
			a32::decode_profile(build_options_),
			block_instruction_count);
		if (output.units == 0) {
			/* A T32 instruction whose second halfword lies on a page
			 * that cannot be fetched: the fetch fault is at that
			 * halfword. */
			return fetch_fault_after(tlb_, memory, address + count * unit);
		}
		const auto sweeps = cache_diagnostics_.link_sweeps;
		selected.emplace(programs_->store_validated(key, output.result,
			bytes.first(output.units * unit), cache_changes_,
			cache_diagnostics_));
		if (cache_diagnostics_.link_sweeps != sweeps)
			forget_returns(cpu);
	}
	const auto &built = *selected;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	cpu.gadget_target_tier = programs_->target_tier();
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	return finish_a32_block(*programs_, event, cpu, memory, executed);
}

/* One instruction, uncached, with the ITSTATE it is entered with. */
::uml::cpu::EngineEvent GadgetExecutor::run_a32_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	cpu.gadget_indirect_miss = nullptr;
	cpu.gadget_target_tier = nullptr;
	const auto key = cpu.pc;
	const auto isa = arm64::aarch32::key_isa(key);
	const auto unit = arm64::aarch32::isa_unit_bytes(isa);
	/* Two halfwords: a T32 instruction is one or two. */
	std::array<std::uint32_t, 1> window;
	const auto bytes = std::as_writable_bytes(std::span{window});
	const auto address = arm64::aarch32::key_pc(key);
	const auto count = fetch_units(memory, address, unit, bytes);
	if (count == 0) {
		return fetch_fault(tlb_, memory);
	}
	const auto output = arm64::gadgets::a32::build(
		{.key = key, .code = bytes.first(count * unit),
		 .it = arm64::aarch32::ItState{cpu.a32_itstate}, .single = true},
		{}, std::pmr::get_default_resource(), {}, build_options_,
		a32::decode_profile(build_options_),
		block_instruction_count);
	if (output.units == 0) {
		return fetch_fault_after(tlb_, memory, address + count * unit);
	}
	const auto &built = output.result;
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	const auto event = arm64::gadgets::execute(
		built.program, cpu, tlb_, executed);
	/* A memory gadget without a key reports its position, which is 0. */
	if ((cpu.pc & 3U) == 1U)
		cpu.pc = key;
	/* The one instruction is the whole program: a terminal that did not
	 * retire gives it back. */
	if (terminal_does_not_retire(event))
		executed = 0;
	cpu.gadget_indirect_miss = nullptr;
	if (event == ::uml::cpu::EngineEvent::continued ||
	    event == ::uml::cpu::EngineEvent::interrupt ||
	    event == ::uml::cpu::EngineEvent::system_call)
		cpu.a32_itstate = output.next_it.bits();
	if (event == ::uml::cpu::EngineEvent::breakpoint)
		breakpoint_immediate_ =
			arm64::aarch32::read_breakpoint_immediate(memory, cpu.pc);
	/* A one-instruction fallthrough ends in the synthetic interrupt gadget:
	 * an internal cold boundary, not a guest-visible interrupt. */
	return event == ::uml::cpu::EngineEvent::interrupt ?
		::uml::cpu::EngineEvent::continued : event;
}

::uml::cpu::EngineEvent GadgetExecutor::finish_a32_block(
	ProgramCache &programs, ::uml::cpu::EngineEvent event,
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* SVC and BKPT leave the stream they ran from, which is not a cache
	 * cell; so do the exits that did not retire. The block holding it is
	 * found from the stream, because the exit may be in a linked successor. */
	const bool leaves_marker =
		event == ::uml::cpu::EngineEvent::system_call ||
		event == ::uml::cpu::EngineEvent::breakpoint;
	const bool does_not_retire = terminal_does_not_retire(event);
	if (does_not_retire || leaves_marker) {
		if (const auto exit = programs.find_stream_source(
			    cpu.gadget_indirect_miss)) {
			const auto &program = exit->result.program;
			const auto guest = program.guest_address();
			/* The memory gadgets other than VLDM, VSTM and the
			 * structure transfers carry no key: they report the
			 * position of their instruction in the block, which has
			 * bit 0 set where a key has none. */
			if ((cpu.pc & 3U) == 1U) {
				const auto at = static_cast<std::size_t>(cpu.pc >> 2);
				std::optional<std::size_t> unit;
				if (arm64::aarch32::key_is_thumb(guest))
					unit = a32::thumb_unit(exit->source, at);
				else if (at < program.instruction_count())
					unit = at;
				if (unit)
					cpu.pc = guest + *unit * 4U;
			}
			const auto position_key = cpu.pc;
			if (position_key >= guest &&
			    ((position_key - guest) & 3U) == 0U) {
				const auto unit = static_cast<std::size_t>(
					(position_key - guest) / 4U);
				std::optional<std::size_t> position;
				if (arm64::aarch32::key_is_thumb(guest)) {
					position = a32::thumb_position(
						exit->source, unit);
					cpu.a32_itstate = a32::thumb_itstate(
						exit->source, unit,
						a32::decode_profile(build_options_));
				} else if (unit < program.instruction_count()) {
					position = unit;
				}
				if (does_not_retire && position &&
				    *position < program.instruction_count()) {
					const auto unretired =
						program.instruction_count() - *position;
					if (executed >= unretired)
						executed -= unretired;
				}
			}
		}
		cpu.gadget_indirect_miss = nullptr;
	}
	if (event == ::uml::cpu::EngineEvent::breakpoint)
		breakpoint_immediate_ =
			arm64::aarch32::read_breakpoint_immediate(memory, cpu.pc);
#if UML_GADGET_EXECUTION_HISTOGRAM
	note_exit_histogram(cache_diagnostics_, programs, event, cpu, executed);
#endif
	if (event == ::uml::cpu::EngineEvent::continued &&
	    cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs.learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_trap_enabled_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* Guest trap enables and single steps are intentionally a cold
	 * execution mode.  Limiting every translated run to one instruction
	 * guarantees that an integer or memory instruction cannot carry
	 * execution past a later FP/SIMD data-processing instruction in the
	 * same cached block, or past the instruction being stepped.
	 */
	cpu.gadget_indirect_miss = nullptr;
	/* Its one-instruction builds name no link state, but no stale table
	 * may be left published either. */
	cpu.gadget_target_tier = nullptr;
	arm64::gadgets::restore_native_fp_environment(cpu);
	std::uint32_t instruction = 0;
	auto encoded = std::as_writable_bytes(std::span{&instruction, 1});
	if (!memory.fetch(cpu.pc, encoded)) {
		return fetch_fault(tlb_, memory);
	}

	auto options = build_options_;
	options.svcr = static_cast<std::uint8_t>(cpu.svcr & arm64::svcr_mask);
	auto built = arm64::gadgets::Builder{}.build(
		cpu.pc, std::span{&instruction, 1}, {},
		std::pmr::get_default_resource(), {}, options);
	if (!built.complete || built.program.instruction_count() != 1) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	if (options.svcr != 0U) {
		cpu.access_sme_state();
		if ((options.svcr & arm64::svcr_sm) != 0U) {
			/* Floating-point exceptions never trap in streaming
			 * mode, so there is no transaction to run. */
			if (!build_options_.host_capabilities.sme_hardware)
				return ::uml::cpu::EngineEvent::fatal;
			cpu.access_fp_state();
			const auto event = arm64::gadgets::
				execute_streaming_preserving_native_fp_environment(
					built.program, cpu, tlb_, executed,
					arm64::sme_profile(
						build_options_.host_capabilities) ==
						arm64::SmeProfile::sme2);
			arm64::gadgets::restore_native_fp_environment(cpu);
			correct_terminal_instruction_count(
				built.program, event, cpu.pc, executed);
			cpu.gadget_indirect_miss = nullptr;
			return event == ::uml::cpu::EngineEvent::interrupt ?
				::uml::cpu::EngineEvent::continued : event;
		}
	}

	const bool transactional =
		arm64::detail::needs_floating_exception_transaction(
			cpu.effective_fpcr(),
			instruction);
	if (!transactional) {
		const auto event = arm64::gadgets::execute(
			built.program, cpu, tlb_, executed);
		correct_terminal_instruction_count(
			built.program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
		/* A one-instruction fallthrough ends in Builder's synthetic
		 * interrupt gadget.  It is an internal cold-mode boundary, not a
		 * guest-visible fairness interrupt.
		 */
		return event == ::uml::cpu::EngineEvent::interrupt ?
			::uml::cpu::EngineEvent::continued : event;
	}

	cpu.access_fp_state();
	const auto original_fpsr = cpu.fpsr;
	arm64::CpuState isolated = cpu;
	isolated.fpsr &=
		~arm64::detail::fpsr_exception_cumulative_mask;

	std::size_t isolated_executed = 0;
	const auto event = arm64::gadgets::execute(
		built.program, isolated, tlb_, isolated_executed);
	if (event != ::uml::cpu::EngineEvent::interrupt &&
	    event != ::uml::cpu::EngineEvent::continued) {
		/* Reserved allocations and other non-executing events retain the
		 * live architectural state.
		 */
		correct_terminal_instruction_count(
			built.program, event, isolated.pc, isolated_executed);
		isolated.gadget_indirect_miss = nullptr;
		executed = isolated_executed;
		return event;
	}

	const auto decision = arm64::detail::decide_a64_floating_exception(
		cpu.fpcr, original_fpsr, isolated.fpsr, instruction,
		cpu.vectors[(instruction >> 5U) & 31U].bytes,
		isolated.vectors[instruction & 31U].bytes);
	if (decision.traps()) {
		cpu.fpsr = decision.committed_fpsr;
		floating_point_syndrome_ = decision.syndrome;
		executed = 0;
		return ::uml::cpu::EngineEvent::floating_point_trap;
	}

	/* The scratch execution began with cumulative flags clear.  Preserve
	 * every other FPSR field produced by the gadget while restoring the
	 * pre-existing cumulative state before committing the transaction.
	 */
	isolated.fpsr =
		(isolated.fpsr &
		 ~arm64::detail::fpsr_exception_cumulative_mask) |
		(decision.committed_fpsr &
		 arm64::detail::fpsr_exception_cumulative_mask);
	cpu = isolated;
	executed = isolated_executed;
	return ::uml::cpu::EngineEvent::continued;
}

bool GadgetExecutor::fault_alignment() const noexcept
{
	return tlb_.fault_alignment != 0;
}

std::uint64_t GadgetExecutor::fault_address() const noexcept
{
	return tlb_.fault_address;
}

::uml::memory::Access GadgetExecutor::fault_access() const noexcept
{
	return tlb_.fault_access;
}

std::uint32_t GadgetExecutor::floating_point_syndrome() const noexcept
{
	return floating_point_syndrome_;
}

#if UML_GADGET_SWEEP_STRESS
namespace stress {

std::size_t live_spaces() noexcept
{
	return stress_live_spaces.load(std::memory_order_relaxed);
}

std::uint64_t closes() noexcept
{
	return stress_closes.load(std::memory_order_relaxed);
}

void set_renewal_period(unsigned stores) noexcept
{
	stress_renewal_period.store(stores, std::memory_order_relaxed);
}

void set_renewal_floor(std::size_t bytes) noexcept
{
	stress_renewal_floor.store(bytes, std::memory_order_relaxed);
}

std::size_t close_spaces(unsigned long generation)
{
	auto &registry = SpaceRegistry::instance();
	std::size_t closed = 0;
	for (auto *space : registry.open_spaces(generation)) {
		registry.close(space);
		++closed;
	}
	return closed;
}

SpaceBytes open_space_bytes(unsigned long generation)
{
	SpaceBytes total{};
	for (const auto *space :
	     SpaceRegistry::instance().open_spaces(generation)) {
		total.arena += space->stress_arena_bytes();
		total.held += space->stress_held_bytes();
		++total.spaces;
	}
	return total;
}

} // namespace stress
#endif

} // namespace uml::cpu::arm64::gadgets
