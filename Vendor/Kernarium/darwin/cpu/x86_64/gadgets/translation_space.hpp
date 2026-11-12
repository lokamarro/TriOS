// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "executor.hpp"
#include "native_symbols.hpp"
#include "program.hpp"
#include "translation_body.hpp"

#include "cpu/gadget_body_arena.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"
#include "cpu/gadget_shared_space.hpp"
#include "cpu/x86_64/code_page_registry.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

[[nodiscard]] constexpr std::size_t mode_index(ExecutionMode mode) noexcept
{
	return mode == ExecutionMode::compat32 ? 1U : 0U;
}

/* The unreachable guest address an untrained indirect cell and an empty
 * target-table entry name. */
[[nodiscard]] constexpr std::uint64_t empty_target(ExecutionMode mode) noexcept
{
	return normalize_instruction_pointer(mode, ~std::uint64_t{});
}

/* A target table nothing has been written to: every entry names an address no
 * block begins at and carries that address's own fallback, so the translated
 * tier can load the pointer before it has compared the address, and a guest
 * that really does branch there still leaves with the right RIP. */
[[nodiscard]] const TargetLinkTable &vacant_target_links() noexcept;

/* The code pages a program's source covers, in address order. */
struct ProgramPages final {
	std::array<std::uint64_t, 2> pages{};
	std::size_t count{};
};
[[nodiscard]] ProgramPages program_pages(const Program &program) noexcept;
/* Whether a store of `size` bytes at `address` overlaps the program's source. */
[[nodiscard]] bool source_overlaps(const Program &program,
	std::uint64_t address, std::size_t size) noexcept;

/* A link as translated code follows it, with what a space needs to find and
 * retire it. Laid out as an UnlinkedBlockLink, so an unlinked record's stream
 * is its own link_exit word, and the pair a link is retargeted by is one aligned
 * sixteen bytes. */
struct alignas(16) LinkRecord final {
	explicit LinkRecord(std::uint64_t address) noexcept
		: unlinked_begin{address}, unlinked_end{address},
		  fallback_stream{reinterpret_cast<std::uintptr_t>(
			native::uml_x86_64_gadget_link_exit)},
		  link{&fallback_stream, 0, address}
	{}

	/* Whether the record links to `body`'s block. */
	[[nodiscard]] bool names(const Body &body) const noexcept
	{
		return link.stream == body.program.native_stream().data();
	}

	void link_to(const Body &body) noexcept
	{
		/* The body's words and bounds are written before the pair that
		 * makes them reachable. */
		std::atomic_thread_fence(std::memory_order_release);
		gadget_cache::store_pair(&link.stream,
			gadget_cache::word_of(body.program.native_stream().data()),
			body.program.instruction_count());
	}

	void reset() noexcept
	{
		gadget_cache::store_pair(&link.stream,
			gadget_cache::word_of(&fallback_stream), 0);
	}

	/* The body a miss may adopt: the latest stored for this address, while no
	 * store has landed over it. Guarded by the record's shard. */
	Body *indexed{};
	std::uint64_t unlinked_begin;
	std::uint64_t unlinked_end;
	std::uintptr_t fallback_stream;
	BlockLink link;
	bool listed{};
};
static_assert(offsetof(LinkRecord, unlinked_begin) ==
		offsetof(UnlinkedBlockLink, unlinked_begin) &&
	offsetof(LinkRecord, fallback_stream) ==
		offsetof(UnlinkedBlockLink, fallback_stream) &&
	offsetof(LinkRecord, link) == offsetof(UnlinkedBlockLink, link) &&
	sizeof(LinkRecord) == 64,
	"a link record is laid out as the unlinked state link_exit and "
	"follow_block_link read");

struct LinkKey final {
	[[nodiscard]] std::uint64_t operator()(
		const LinkRecord &record) const noexcept
	{
		return record.link.guest_rip;
	}
};

/* An indirect site is named by the branch that owns it, which its own cells do
 * not record. Translated streams point at the site, the first member. */
struct alignas(16) IndirectRecord final {
	explicit IndirectRecord(std::uint64_t source) noexcept
		: source_rip{source}
	{}

	IndirectLink site{};
	std::uint64_t source_rip;
};
static_assert(offsetof(IndirectRecord, site) == 0 &&
	sizeof(IndirectRecord) % 16 == 0,
	"a site's cells are aligned pairs");

struct IndirectKey final {
	[[nodiscard]] std::uint64_t operator()(
		const IndirectRecord &record) const noexcept
	{
		return record.source_rip;
	}
};

/* A shared address space whose arenas hold at least this many bytes that no
 * body needs, and more of them than its bodies hold, is renewed. */
inline constexpr std::size_t renewal_floor_bytes = std::size_t{2} << 20;

/* What makes two executors' translations interchangeable: the address space,
 * the exceptions the guest enabled (a block is built so that none of its
 * instructions needs a check the runtime makes itself) and the build options.
 * A key with an owner is private to that cache, which `owner` points at the
 * flag of: the flag is set when the byte budget asks the space to shed. */
struct SpaceKey final {
	unsigned long generation{};
	std::uint8_t mode{};
	const void *owner{};
	std::uint8_t options{};
};

/* What an executor reads of the pages its build will cover, before it reads a
 * byte of them: how many stores to each the address space has seen. A body
 * built across a store is born stale, and is not stored. */
struct PagePins final {
	std::array<std::uint64_t, 2> pages{};
	std::array<std::uint64_t, 2> stores{};
	std::size_t count{};
};

/* One address space's translations.
 *
 * Its bodies, the link records and indirect sites their streams name, and the
 * target tables the indirect gadgets consult all belong to the address space,
 * so every executor running it shares them. Each executor keeps its own tagged
 * slots, which decide what it holds; a body lives while any slot holds it, so
 * the address space costs the union of what its executors hold rather than the
 * sum. A miss on a body another executor holds adopts it after validation
 * against guest memory, with no copy or relocation.
 *
 * The records are split eight ways by key, each part behind its own lock, and
 * so are the pages bodies cover. Lock order: registry, reset, page shard, link
 * shard, limbo, arena; the registry's lock also covers a space's attached list.
 *
 * A body a store has landed over is stale: no link is armed to it, and a slot
 * that finds it discards it. A store marks the bodies it overlaps, under the
 * lock of their page, and resets the record of each, so that a link another
 * executor armed after validating the body's bytes, and before the store, does
 * not take the writer into bytes from before its own store.
 *
 * Code changes that are not the writer's own -- a mapping, a store the kernel
 * made, a JIT's -- reach the space through observe(): the first executor to
 * see one raises the gate and resets the links of the bodies the change
 * covers, and an activation validated before it is refused.
 *
 * A shared space is never swept. It counts what its arenas hold and what its
 * bodies use of it; once the arenas hold at least as much freed space as bodies
 * (and at least renewal_floor_bytes) the space is closed: it leaves the
 * registry, so the next executor to want its address space gets a new one,
 * and every executor attached lets go of it before it next enters translated
 * code. The old space dies with its last executor, taking its arenas, link
 * records and freed bodies with it.
 *
 * An address space that one cache keeps to itself (a key with an owner) is
 * swept by that cache, and nothing else is shared; every space charges its
 * arenas, record tables and target tables to an account in the process's
 * gadget-cache budget (gadget_cache_budget.hpp), which may ask every executor
 * attached to let go of it.
 */
class Space final {
public:
	using LinkTable = gadget_cache::StableRecordTable<LinkRecord, LinkKey>;
	using SiteTable =
		gadget_cache::StableRecordTable<IndirectRecord, IndirectKey>;
	using LinkMarks = LinkTable::Marks;
	using SiteMarks = SiteTable::Marks;
	using Participant = CodePageRegistry::Participant;

	Space(SpaceKey key, std::uint64_t changes);
	/* No executor holds this address space any more, so nothing can run a
	 * body or follow a record of it, and the bodies still waiting in limbo
	 * go with the arenas: they unmap their chunks whole, which costs what
	 * they mapped rather than what they hold. */
	~Space();

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	[[nodiscard]] const SpaceKey &key() const noexcept { return key_; }
	[[nodiscard]] bool shared() const noexcept
	{
		return key_.owner == nullptr;
	}

	/* The registry's view of the space (gadget_cache::SpaceRegistry). */
	void attach(const gadget_cache::ExecutorAnnouncement *announcement)
	{
		attached_.attach(announcement);
	}
	/* Whether this was the last executor. */
	[[nodiscard]] bool detach(
		const gadget_cache::ExecutorAnnouncement *announcement)
	{
		return attached_.detach(announcement);
	}
	/* Close the space; true for the caller that did. */
	[[nodiscard]] bool close() noexcept { return ledger_.close(); }
	[[nodiscard]] bool closed() const noexcept { return ledger_.closed(); }
	/* Whether the arena holds enough freed space to be replaced, and the
	 * space has not been (see renewal_floor_bytes). Only a shared space
	 * is ever renewed: a private one is swept. */
	[[nodiscard]] bool renewal_due() const noexcept
	{
		return ledger_.renewal_due();
	}
	/* Whether the gadget-cache budget asked this space to shed. */
	[[nodiscard]] bool shedding() const noexcept
	{
		return account_.shedding();
	}
	/* Ask every executor attached to let go of this address space before
	 * it next enters translated code. */
	void request_renewal();
	/* A shared space was closed, for renewal or because the budget asked it
	 * to shed (the registry calls this once for each); how many so far. */
	static void note_closed() noexcept
	{
		closes().fetch_add(1, std::memory_order_relaxed);
	}
	[[nodiscard]] static std::uint64_t closed_spaces() noexcept
	{
		return closes().load(std::memory_order_relaxed);
	}

	[[nodiscard]] gadget_cache::Account &account() noexcept
	{
		return account_;
	}
	[[nodiscard]] const gadget_cache::Account &account() const noexcept
	{
		return account_;
	}

	/* ---- Bodies ---- */

	/* The pages a build will cover, held in the page index until unpinned,
	 * with the stores each has seen. Before the registry counts them, or any
	 * of their bytes is read. */
	[[nodiscard]] PagePins pin(std::span<const std::uint64_t> pages);
	void unpin(const PagePins &pins) noexcept;
	/* Whether some body of the space has source on `page`. */
	[[nodiscard]] bool holds_page(std::uint64_t page) const;

	/* Store a freshly built translation, or adopt the equal one another
	 * executor stored first, and hold it for the caller's slot. Null when a
	 * store landed on its pages since they were pinned, in which case the
	 * program is not worth keeping. Throws when the memory is not there. */
	[[nodiscard]] Body *publish(const Program &program,
		std::uint8_t instruction_limit, const ProgramCodeStamps &stamps,
		const PagePins &pins, const Participant *self);
	/* Hold the body indexed for `address` if a request for at most
	 * `instruction_limit` instructions can use it, for the caller to
	 * validate. */
	[[nodiscard]] Body *hold_indexed(std::uint64_t address,
		ExecutionMode mode, std::uint8_t instruction_limit);
	/* A slot lets go of `body`. `stale` says validation found guest code
	 * other than the body's, so no miss should adopt it any more. */
	void release(Body *body, bool stale) noexcept;
	/* Free what was unpublished long enough ago that no other executor can
	 * still be running it. Never waits. */
	void reclaim(const gadget_cache::ExecutorAnnouncement *self) noexcept;
	/* The live body whose allocation holds `address`, or null. A body
	 * waiting in limbo is still live here, so an exit inside a successor that
	 * was unpublished while it ran is still attributed to it. */
	[[nodiscard]] const Body *owner(const void *address) const noexcept;

	/* ---- Links ---- */

	[[nodiscard]] const BlockLink *link(
		std::uint64_t address, ExecutionMode mode);
	/* The site of the branch at `source`, made on first use, with its cells
	 * empty. */
	[[nodiscard]] IndirectLink *indirect_link(
		std::uint64_t source, ExecutionMode mode);
	/* Teach `site` that its branch reached `target`. */
	void learn_indirect(
		IndirectLink &site, std::uint64_t target, ExecutionMode mode);
	/* The table an entering block probes, made on first use, or the vacant
	 * one when it cannot be made. */
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept;

	/* Link the record for the block `body` holds, unless a code change has
	 * been published since the caller validated the body at `changes`, or a
	 * store has landed over it. Returns the record, for the slot's memo, and
	 * announces the block in the target table. */
	LinkRecord *activate(LinkRecord *memo, std::uint64_t address,
		ExecutionMode mode, const Body &body, std::uint64_t changes);

	/* ---- Code changes ---- */

	/* A store of `size` bytes landed at `address`: mark every body it
	 * overlaps stale and reset its record. */
	void note_store(std::uint64_t address, std::size_t size);
	/* Answer a code change published up to `changes`: before any executor
	 * runs the address space at `changes`, the records of the bodies the
	 * change covers are reset, every one when `reader` cannot describe it. */
	void observe(std::uint64_t changes, ProgramCacheCodeChangeReader reader,
		void *context);
	/* The executor that published the changes up to `changes` itself has
	 * accepted them at a branch, having found no other in the sequence: the
	 * bodies each of its stores overlapped were marked, and their records
	 * reset, as it stored, so nothing more is reset, and the gate only
	 * moves to where the executor is. Others reach `changes` through
	 * observe(), which finds it done. */
	void accept(std::uint64_t changes);

	/* ---- Private spaces ---- */

	/* Records and sites, and what the last sweep kept and the bodies that
	 * have left since: a sweep waits for enough of them
	 * (gadget_cache::sweep_due). Only a space one cache keeps to itself is
	 * swept: that cache's slots are everything that holds one of its
	 * bodies, and none of its translated code runs while it stores. */
	[[nodiscard]] bool sweep_due() const noexcept;
	void note_departed() noexcept
	{
		departed_.fetch_add(1, std::memory_order_relaxed);
	}
	[[nodiscard]] std::array<LinkMarks, 2> link_marks() const;
	[[nodiscard]] std::array<SiteMarks, 2> site_marks() const;
	/* The record for `address`, or null before anything made one. */
	[[nodiscard]] const LinkRecord *find_link(
		std::uint64_t address, ExecutionMode mode) const;

	struct Released final {
		std::size_t links;
		std::size_t sites;
	};
	/* Release every record and site the marks leave. A site cell or target
	 * table entry may still name such a record, whose block is gone; it is
	 * emptied as a code change empties it, to learn its target again if
	 * that runs. Everything this allocates comes before anything is
	 * released, and each step after leaves the space consistent, so a
	 * sweep that runs out of memory throws before it changes anything. */
	[[nodiscard]] Released sweep(const std::array<LinkMarks, 2> &links,
		const std::array<SiteMarks, 2> &sites);
	/* A sweep, whole or not, has run: the next waits for growth and
	 * departures from here. */
	void swept() noexcept;

	/* The record a link belongs to. */
	[[nodiscard]] static const LinkRecord *record_of(
		const BlockLink *link) noexcept
	{
		return reinterpret_cast<const LinkRecord *>(
			reinterpret_cast<std::uintptr_t>(link) -
			offsetof(LinkRecord, link));
	}

#if UML_GADGET_CACHE_CENSUS
	/* What the space keeps besides its bodies, in bytes. */
	struct LinkBytes final {
		std::size_t links, indirect, targets, pages;
	};
	[[nodiscard]] LinkBytes link_bytes() const;
	[[nodiscard]] std::size_t link_records() const;
	[[nodiscard]] std::size_t attached() const { return attached_.size(); }
	[[nodiscard]] std::size_t limbo_bodies() const { return limbo_.size(); }
	/* The bytes the arenas hold, and of those the ones no body uses. */
	[[nodiscard]] std::size_t stream_index_bytes() const;
	[[nodiscard]] std::size_t stream_bytes() const;
	[[nodiscard]] std::size_t empty_bytes() const;
	template <typename Visit>
	void for_each_link(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			for (const auto &mode : shard.links)
				mode.for_each(visit);
		}
	}
	template <typename Visit>
	void for_each_indirect(Visit &&visit) const
	{
		for (const auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			for (const auto &mode : shard.indirect)
				mode.for_each(visit);
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
		std::array<LinkTable, 2> links;
		std::array<SiteTable, 2> indirect;
		/* The records and sites holding an activation. */
		std::vector<LinkRecord *> live_links;
		std::array<std::vector<IndirectLink *>, 2> live_indirect;
	};

	/* What the address space knows of a page: its bodies, as a list
	 * through the bodies, how many stores have landed on it since the
	 * space has watched it, and how many builds have it pinned. */
	struct PageEntry final {
		Body *first{};
		std::uint64_t stores{};
		std::uint32_t pins{};
	};
	struct PageShard final {
		mutable std::mutex mutex;
		std::unordered_map<std::uint64_t, PageEntry> pages;
	};

	[[nodiscard]] static std::size_t shard_index(
		ExecutionMode mode, std::uint64_t key) noexcept
	{
		auto mixed = (key ^ (std::uint64_t{mode_index(mode)} << 63)) *
			0x9e3779b97f4a7c15ULL;
		mixed ^= mixed >> 29;
		return (mixed >> 7) & (shard_count - 1U);
	}
	[[nodiscard]] Shard &shard_of(ExecutionMode mode, std::uint64_t key)
	{
		return shards_[shard_index(mode, key)];
	}
	[[nodiscard]] const Shard &shard_of(
		ExecutionMode mode, std::uint64_t key) const
	{
		return shards_[shard_index(mode, key)];
	}
	[[nodiscard]] static std::size_t page_shard_index(
		std::uint64_t page) noexcept
	{
		return static_cast<std::size_t>(
			((page >> 12U) * 0x9e3779b97f4a7c15ULL) >> 32U) &
			(shard_count - 1U);
	}
	[[nodiscard]] PageShard &page_shard_of(std::uint64_t page)
	{
		return page_shards_[page_shard_index(page)];
	}
	[[nodiscard]] const PageShard &page_shard_of(std::uint64_t page) const
	{
		return page_shards_[page_shard_index(page)];
	}

	/* Which of a body's two page entries a page is. A body's entries are
	 * distinct pages, the first of which is the page of its first byte. */
	[[nodiscard]] static std::size_t page_entry(
		const Body &body, std::uint64_t page) noexcept;
	[[nodiscard]] static std::uint64_t cell_rip(
		IndirectLinkEntry &entry) noexcept;
	[[nodiscard]] static bool untrained(
		IndirectLink &site, std::uint64_t empty) noexcept;
	/* The indexed body when it stands for exactly this program. */
	[[nodiscard]] static Body *equal_indexed(const LinkRecord &record,
		const Program &program, std::uint8_t instruction_limit) noexcept;
	[[nodiscard]] Body *make_body(const Program &program,
		std::uint8_t instruction_limit, const ProgramCodeStamps &stamps);
	void free_bodies(std::vector<Body *> &bodies) noexcept;
	void free_body(Body *body) noexcept;
	/* What a body takes from the arenas, with its source. */
	[[nodiscard]] static std::size_t body_block_bytes(
		const Body &body) noexcept;
	/* The arenas' bytes changed hands: record them for the renewal ledger of
	 * a shared space, and mark the space when they grew and are mostly
	 * freed. The caller holds arena_mutex_. */
	bool note_arena() noexcept;
	[[nodiscard]] TargetLinkTable *target_table(ExecutionMode mode) noexcept;
	void train_target(ExecutionMode mode, std::uint64_t rip,
		const BlockLink *target) noexcept;
	/* Put a body in, and take it out of, the lists of its pages. The page
	 * shards are locked. */
	void index_body(Body &body, const ProgramPages &pages,
		ProgramPages &gained);
	void unindex_body(Body &body, const ProgramPages &pages,
		ProgramPages &lost) noexcept;
	/* Reset the record of a body that a store or a code change covers, if it
	 * names the body; `clear_indexed` also stops a miss adopting it. The
	 * caller holds the body's page shard, and this takes its link shard. */
	void reset_record_of(Body &body, bool clear_indexed) noexcept;
	void reset_all() noexcept;
	[[nodiscard]] std::size_t records() const noexcept;

	static void shed(void *owner) noexcept;
	[[nodiscard]] static std::atomic<std::uint64_t> &closes() noexcept
	{
		static std::atomic<std::uint64_t> count;
		return count;
	}

	SpaceKey key_;
	std::array<Shard, shard_count> shards_;
	std::array<PageShard, shard_count> page_shards_;
	std::shared_ptr<CodePageRegistry> registry_;
	std::array<const BlockLink *, 2> empty_link_{};
	/* The code-change sequence links may be activated at, and the one whose
	 * reset has finished. */
	std::atomic<std::uint64_t> gate_;
	std::atomic<std::uint64_t> reset_done_;
	std::mutex reset_mutex_;
	std::mutex targets_mutex_;
	std::array<std::unique_ptr<TargetLinkTable>, 2> targets_storage_;
	std::array<std::atomic<TargetLinkTable *>, 2> targets_{};
	gadget_cache::Limbo<Body> limbo_;
	/* The translations of the address space: each body with its stream, and
	 * apart from them the source each was read from, which is cold. Both are
	 * charged to the account as they grow and shrink. The arenas are not
	 * safe to share, so every use of them is behind arena_mutex_. */
	mutable std::mutex arena_mutex_;
	gadget_cache::BodyArena streams_{std::pmr::new_delete_resource()};
	gadget_cache::BodyArena sources_{std::pmr::new_delete_resource()};
	gadget_cache::AttachedExecutors attached_;
	/* What decides a shared space is renewed, and that it was. */
	gadget_cache::RenewalLedger ledger_;
	/* For a private address space: the records and sites its last sweep
	 * kept, and the bodies that have left it since (sweep_due()). */
	std::size_t kept_{};
	std::atomic<std::size_t> departed_{};
	/* Last, so it is destroyed first: a call of shed() in progress holds up
	 * its destruction, and everything shed() touches is still there. */
	gadget_cache::Account account_;
};

/* The address spaces whose translations executors share, by key. */
using SpaceRegistry = gadget_cache::SpaceRegistry<Space, SpaceKey>;

} // namespace uml::cpu::x86_64::gadgets
