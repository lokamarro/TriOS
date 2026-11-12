// SPDX-License-Identifier: GPL-2.0-or-later
#include "translation_space.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

#include <malloc/malloc.h>

namespace uml::cpu::x86_64::gadgets {
namespace {

constexpr std::uint64_t page_mask =
	~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};

/* A prepare that would walk more than this replays nothing and resets every
 * link of the address space instead, which is cheaper than the walk. */
constexpr std::size_t code_change_replay_limit = 64;
constexpr std::uint64_t code_change_replay_pages = 128;

constexpr std::array<ExecutionMode, 2> modes{
	ExecutionMode::long64, ExecutionMode::compat32};

/* The page shards a program's pages are in, locked in index order. */
class PageLocks final {
public:
	template <typename Shards>
	PageLocks(Shards &shards, const std::array<std::size_t, 2> &indexes,
		std::size_t count)
	{
		if (count == 0)
			return;
		auto first = indexes[0];
		auto second = count == 2 ? indexes[1] : indexes[0];
		if (second < first)
			std::swap(first, second);
		first_ = std::unique_lock{shards[first].mutex};
		if (second != first)
			second_ = std::unique_lock{shards[second].mutex};
	}

private:
	std::unique_lock<std::mutex> first_;
	std::unique_lock<std::mutex> second_;
};

} // namespace

const TargetLinkTable &vacant_target_links() noexcept
{
	static const UnlinkedBlockLink vacant{
		0, ~std::uint64_t{}, ~std::uint64_t{},
		reinterpret_cast<std::uintptr_t>(
			native::uml_x86_64_gadget_link_exit),
		{&vacant.fallback_stream, 0, ~std::uint64_t{}},
	};
	static const TargetLinkTable table = [] {
		TargetLinkTable initial{};
		for (auto &entry : initial)
			entry = {~std::uint64_t{}, &vacant.link};
		return initial;
	}();
	return table;
}

ProgramPages program_pages(const Program &program) noexcept
{
	ProgramPages result;
	auto current = program.address();
	auto remaining = program.byte_count();
	while (remaining != 0) {
		const auto page = current & page_mask;
		if (result.count == 0 || result.pages[result.count - 1U] != page)
			result.pages[result.count++] = page;
		const auto count = std::min<std::size_t>(remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(current & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		remaining -= count;
		current = advance_instruction_pointer(
			program.execution_mode(), current, count);
	}
	return result;
}

bool source_overlaps(const Program &program, std::uint64_t address,
	std::size_t size) noexcept
{
	auto source = program.address();
	auto source_remaining = program.byte_count();
	while (source_remaining != 0) {
		const auto source_page = source & page_mask;
		const auto source_count = std::min<std::size_t>(source_remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(source & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		auto written = address;
		auto written_remaining = size;
		while (written_remaining != 0) {
			const auto written_page = written & page_mask;
			const auto written_count = std::min<std::size_t>(
				written_remaining, DARWIN_UML_GUEST_PAGE_SIZE -
					(written & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
			if (source_page == written_page && source <
				    written + written_count && written <
				    source + source_count)
				return true;
			written_remaining -= written_count;
			written += written_count;
		}
		source_remaining -= source_count;
		source = advance_instruction_pointer(
			program.execution_mode(), source, source_count);
	}
	return false;
}

Space::Space(SpaceKey key, std::uint64_t changes)
	: key_{key}, registry_{CodePageRegistry::acquire(key.generation)},
	  gate_{changes}, reset_done_{changes}, account_{shed, this}
{
	streams_.meter(&account_);
	sources_.meter(&account_);
	for (auto &shard : shards_) {
		for (auto &mode : shard.links)
			mode.meter(&account_);
		for (auto &mode : shard.indirect)
			mode.meter(&account_);
	}
	for (const auto mode : modes) {
		const auto empty = empty_target(mode);
		auto &shard = shard_of(mode, empty);
		empty_link_[mode_index(mode)] =
			&shard.links[mode_index(mode)].try_emplace(empty, empty)
				 .first->link;
	}
}

Space::~Space()
{
	/* What a replay decoded was allocated apart from the arena, which
	 * goes whole; the bodies that were waiting for executors to leave are
	 * the only ones left to give it back. */
	limbo_.for_each([](Body *body) { std::destroy_at(body); });
	/* Every executor let go of its bodies before the last of them detached,
	 * so no page is held, and none is pinned. */
	assert(std::all_of(page_shards_.begin(), page_shards_.end(),
		[](const PageShard &shard) {
			return std::all_of(shard.pages.begin(), shard.pages.end(),
				[](const auto &page) {
					return page.second.first == nullptr;
				});
		}));
}

void Space::shed(void *owner) noexcept
{
	try {
		static_cast<Space *>(owner)->request_renewal();
	} catch (...) {
		/* The space is still shedding: the next executor to want its
		 * address space closes it instead of attaching. */
	}
}

void Space::request_renewal()
{
	if (key_.owner != nullptr) {
		static_cast<std::atomic<bool> *>(const_cast<void *>(key_.owner))
			->store(true, std::memory_order_relaxed);
		return;
	}
	attached_.request_renewal();
}

PagePins Space::pin(std::span<const std::uint64_t> pages)
{
	PagePins pins;
	try {
		for (const auto page : pages) {
			assert(pins.count < pins.pages.size());
			auto &shard = page_shard_of(page);
			const std::lock_guard guard{shard.mutex};
			auto &entry = shard.pages.try_emplace(page).first->second;
			++entry.pins;
			pins.pages[pins.count] = page;
			pins.stores[pins.count] = entry.stores;
			++pins.count;
		}
	} catch (...) {
		unpin(pins);
		throw;
	}
	return pins;
}

void Space::unpin(const PagePins &pins) noexcept
{
	for (std::size_t index = 0; index < pins.count; ++index) {
		auto &shard = page_shard_of(pins.pages[index]);
		const std::lock_guard guard{shard.mutex};
		const auto found = shard.pages.find(pins.pages[index]);
		if (found == shard.pages.end())
			continue;
		if (--found->second.pins == 0 && found->second.first == nullptr)
			shard.pages.erase(found);
	}
}

bool Space::holds_page(std::uint64_t page) const
{
	const auto &shard = page_shard_of(page);
	const std::lock_guard guard{shard.mutex};
	const auto found = shard.pages.find(page);
	return found != shard.pages.end() && found->second.first != nullptr;
}

std::size_t Space::page_entry(const Body &body, std::uint64_t page) noexcept
{
	return (body.program.address() & page_mask) == page ? 0U : 1U;
}

Body *Space::equal_indexed(const LinkRecord &record, const Program &program,
	std::uint8_t instruction_limit) noexcept
{
	auto *held = record.indexed;
	if (held == nullptr || held->stale.load(std::memory_order_acquire) ||
	    held->instruction_limit != instruction_limit ||
	    held->program.terminal() != program.terminal())
		return nullptr;
	const auto source = held->program.source();
	const auto wanted = program.source();
	if (source.size() != wanted.size() ||
	    std::memcmp(source.data(), wanted.data(), source.size()) != 0)
		return nullptr;
	return held;
}

Body *Space::make_body(const Program &program,
	std::uint8_t instruction_limit, const ProgramCodeStamps &stamps)
{
	const auto bytes =
		Body::allocation_bytes(program.native_stream().size());
	std::byte *storage = nullptr;
	std::byte *source = nullptr;
	bool grown = false;
	{
		const std::lock_guard arena{arena_mutex_};
		storage = static_cast<std::byte *>(streams_.allocate(bytes));
		try {
			if (program.byte_count() != 0)
				source = static_cast<std::byte *>(
					sources_.allocate(program.byte_count()));
		} catch (...) {
			streams_.deallocate(storage, bytes);
			(void)note_arena();
			throw;
		}
		grown = note_arena();
	}
	if (grown)
		ledger_.note_idle(renewal_floor_bytes);
	auto *body = new (storage) Body{program.view_in(
		reinterpret_cast<std::uintptr_t *>(storage + sizeof(Body)),
		source)};
	body->instruction_limit = instruction_limit;
	body->set_stamps(stamps);
	return body;
}

void Space::free_bodies(std::vector<Body *> &bodies) noexcept
{
	if (bodies.empty())
		return;
	const std::lock_guard arena{arena_mutex_};
	for (auto *body : bodies) {
		const auto bytes = body->allocation_bytes();
		auto *source =
			const_cast<std::byte *>(body->program.source().data());
		const auto source_bytes = body->program.byte_count();
		std::destroy_at(body);
		if (source_bytes != 0)
			sources_.deallocate(source, source_bytes);
		streams_.deallocate(body, bytes);
	}
	(void)note_arena();
	bodies.clear();
}

void Space::free_body(Body *body) noexcept
{
	const std::lock_guard arena{arena_mutex_};
	const auto bytes = body->allocation_bytes();
	auto *source = const_cast<std::byte *>(body->program.source().data());
	const auto source_bytes = body->program.byte_count();
	std::destroy_at(body);
	if (source_bytes != 0)
		sources_.deallocate(source, source_bytes);
	streams_.deallocate(body, bytes);
	(void)note_arena();
}

std::size_t Space::body_block_bytes(const Body &body) noexcept
{
	using gadget_cache::BodyArena;
	return BodyArena::block_bytes(body.allocation_bytes()) +
		(body.program.byte_count() != 0 ?
			BodyArena::block_bytes(body.program.byte_count()) : 0U);
}

bool Space::note_arena() noexcept
{
	if (!shared())
		return false;
	return ledger_.note_arena(streams_.bytes() + sources_.bytes());
}

void Space::index_body(Body &body, const ProgramPages &pages,
	ProgramPages &gained)
{
	for (std::size_t index = 0; index < pages.count; ++index) {
		const auto page = pages.pages[index];
		auto &entry = page_shard_of(page).pages.find(page)->second;
		auto *first = entry.first;
		if (first == nullptr)
			gained.pages[gained.count++] = page;
		body.page_previous[index] = nullptr;
		body.page_next[index] = first;
		if (first != nullptr)
			first->page_previous[page_entry(*first, page)] = &body;
		entry.first = &body;
		body.linked_pages |= static_cast<std::uint8_t>(1U << index);
	}
}

void Space::unindex_body(Body &body, const ProgramPages &pages,
	ProgramPages &lost) noexcept
{
	for (std::size_t index = 0; index < pages.count; ++index) {
		const auto bit = static_cast<std::uint8_t>(1U << index);
		if ((body.linked_pages & bit) == 0)
			continue;
		const auto page = pages.pages[index];
		auto &shard = page_shard_of(page);
		const auto found = shard.pages.find(page);
		auto *previous = body.page_previous[index];
		auto *next = body.page_next[index];
		if (previous != nullptr)
			previous->page_next[page_entry(*previous, page)] = next;
		else if (found != shard.pages.end())
			found->second.first = next;
		if (next != nullptr)
			next->page_previous[page_entry(*next, page)] = previous;
		body.page_previous[index] = nullptr;
		body.page_next[index] = nullptr;
		body.linked_pages &= static_cast<std::uint8_t>(~bit);
		if (previous == nullptr && next == nullptr) {
			lost.pages[lost.count++] = page;
			if (found != shard.pages.end() && found->second.pins == 0)
				shard.pages.erase(found);
		}
	}
}

Body *Space::publish(const Program &program, std::uint8_t instruction_limit,
	const ProgramCodeStamps &stamps, const PagePins &pins,
	const Participant *self)
{
	const auto mode = program.execution_mode();
	const auto address = program.address();
	const auto mode_slot = mode_index(mode);
	auto &shard = shard_of(mode, address);
	{
		const std::lock_guard guard{shard.mutex};
		auto &record =
			*shard.links[mode_slot].try_emplace(address, address).first;
		if (auto *held = equal_indexed(record, program, instruction_limit)) {
			held->holders.fetch_add(1, std::memory_order_relaxed);
			return held;
		}
	}
	auto *body = make_body(program, instruction_limit, stamps);
	const auto pages = program_pages(body->program);
	ProgramPages gained;
	Body *adopted = nullptr;
	bool born_stale = false;
	std::array<std::size_t, 2> indexes{};
	for (std::size_t index = 0; index < pages.count; ++index)
		indexes[index] = page_shard_index(pages.pages[index]);
	try {
		const PageLocks locks{page_shards_, indexes, pages.count};
		/* A store that landed on a page since the build pinned it may have
		 * landed over the bytes it read. */
		for (std::size_t index = 0; index < pages.count && !born_stale;
		     ++index) {
			const auto page = pages.pages[index];
			std::size_t pinned = pins.count;
			for (std::size_t seen = 0; seen < pins.count; ++seen)
				if (pins.pages[seen] == page)
					pinned = seen;
			const auto &shard_pages = page_shard_of(page).pages;
			const auto found = shard_pages.find(page);
			if (pinned == pins.count || found == shard_pages.end() ||
			    found->second.stores != pins.stores[pinned])
				born_stale = true;
		}
		if (!born_stale) {
			const std::lock_guard guard{shard.mutex};
			auto &record = *shard.links[mode_slot]
				.try_emplace(address, address).first;
			if (auto *held = equal_indexed(record, program,
				    instruction_limit)) {
				held->holders.fetch_add(1,
					std::memory_order_relaxed);
				adopted = held;
			} else {
				body->holders.store(1, std::memory_order_relaxed);
				if (shared())
					ledger_.note_held(body_block_bytes(*body));
				record.indexed = body;
				index_body(*body, pages, gained);
			}
		}
	} catch (...) {
		std::vector<Body *> never{body};
		free_bodies(never);
		throw;
	}
	if (born_stale || adopted != nullptr) {
		/* Never published, so nothing can reach it. */
		std::vector<Body *> never{body};
		free_bodies(never);
		return adopted;
	}
	for (std::size_t index = 0; index < gained.count; ++index)
		registry_->add(gained.pages[index], self);
	return body;
}

Body *Space::hold_indexed(std::uint64_t address, ExecutionMode mode,
	std::uint8_t instruction_limit)
{
	auto &shard = shard_of(mode, address);
	const std::lock_guard guard{shard.mutex};
	auto *record = shard.links[mode_index(mode)].find(address);
	if (record == nullptr || record->indexed == nullptr)
		return nullptr;
	auto *body = record->indexed;
	if (body->stale.load(std::memory_order_acquire) ||
	    (!body->program.terminal() &&
		    instruction_limit > body->instruction_limit))
		return nullptr;
	body->holders.fetch_add(1, std::memory_order_relaxed);
	return body;
}

void Space::release(Body *body, bool stale) noexcept
{
	const auto mode = body->program.execution_mode();
	const auto address = body->program.address();
	auto &shard = shard_of(mode, address);
	bool last = false;
	{
		const std::lock_guard guard{shard.mutex};
		auto *record = shard.links[mode_index(mode)].find(address);
		last = body->holders.fetch_sub(1, std::memory_order_acq_rel) == 1;
		if (stale)
			body->stale.store(true, std::memory_order_release);
		if (record != nullptr && record->indexed == body && (last || stale))
			record->indexed = nullptr;
		/* Nothing names it from here on; executors that loaded the record
		 * before this may still be running it, so it waits for them in
		 * limbo. */
		if (last && record != nullptr && record->names(*body))
			record->reset();
		if (last && shared())
			ledger_.note_released(body_block_bytes(*body),
				renewal_floor_bytes);
	}
	if (!last)
		return;
	const auto pages = program_pages(body->program);
	ProgramPages lost;
	std::array<std::size_t, 2> indexes{};
	for (std::size_t index = 0; index < pages.count; ++index)
		indexes[index] = page_shard_index(pages.pages[index]);
	{
		const PageLocks locks{page_shards_, indexes, pages.count};
		unindex_body(*body, pages, lost);
	}
	for (std::size_t index = 0; index < lost.count; ++index)
		registry_->remove(lost.pages[index]);
	if (!shared()) {
		/* Only the cache that keeps this address space to itself can
		 * run its blocks, and it is not running translated code while
		 * it lets go of one. */
		free_body(body);
		return;
	}
	limbo_.push(body);
}

void Space::reclaim(const gadget_cache::ExecutorAnnouncement *self) noexcept
{
	limbo_.reclaim(self, attached_,
		[this](std::vector<Body *> &bodies) noexcept {
			free_bodies(bodies);
		});
}

const Body *Space::owner(const void *address) const noexcept
{
	const std::lock_guard arena{arena_mutex_};
	return static_cast<const Body *>(streams_.owner(address));
}

const BlockLink *Space::link(std::uint64_t address, ExecutionMode mode)
{
	auto &shard = shard_of(mode, address);
	const std::lock_guard guard{shard.mutex};
	return &shard.links[mode_index(mode)].try_emplace(address, address)
			.first->link;
}

const LinkRecord *Space::find_link(
	std::uint64_t address, ExecutionMode mode) const
{
	const auto &shard = shard_of(mode, address);
	const std::lock_guard guard{shard.mutex};
	return shard.links[mode_index(mode)].find(address);
}

std::uint64_t Space::cell_rip(IndirectLinkEntry &entry) noexcept
{
	return std::atomic_ref<std::uint64_t>{entry.guest_rip}.load(
		std::memory_order_relaxed);
}

bool Space::untrained(IndirectLink &site, std::uint64_t empty) noexcept
{
	for (auto &entry : site.entries)
		if (cell_rip(entry) != empty)
			return false;
	return true;
}

IndirectLink *Space::indirect_link(std::uint64_t source, ExecutionMode mode)
{
	auto &shard = shard_of(mode, source);
	const std::lock_guard guard{shard.mutex};
	const auto [created, inserted] =
		shard.indirect[mode_index(mode)].try_emplace(source, source);
	if (inserted) {
		const auto empty = empty_target(mode);
		for (auto &entry : created->site.entries)
			gadget_cache::store_pair(&entry, empty,
				gadget_cache::word_of(empty_link_[mode_index(mode)]));
		created->site.next_replacement = 0;
		created->site.listed = false;
	}
	return &created->site;
}

void Space::learn_indirect(
	IndirectLink &site, std::uint64_t target, ExecutionMode mode)
{
	target = normalize_instruction_pointer(mode, target);
	const auto *target_link = link(target, mode);
	train_target(mode, target, target_link);
	const auto empty = empty_target(mode);
	/* Training is the only thing C++ does to move a site off its empty
	 * cells, so a site still holding two empty cells is untrained since its
	 * last reset -- which makes this the transition that enrols it. */
	if (untrained(site, empty)) {
		const auto &owner =
			*reinterpret_cast<const IndirectRecord *>(&site);
		auto &shard = shard_of(mode, owner.source_rip);
		const std::lock_guard guard{shard.mutex};
		if (untrained(site, empty)) {
			shard.live_indirect[mode_index(mode)].push_back(&site);
			gadget_cache::store_pair(&site.entries[0], target,
				gadget_cache::word_of(target_link));
			return;
		}
	}
	for (auto &entry : site.entries) {
		if (cell_rip(entry) != target)
			continue;
		if (std::atomic_ref<const BlockLink *>{entry.link}.load(
			    std::memory_order_relaxed) != target_link)
			gadget_cache::store_pair(&entry, target,
				gadget_cache::word_of(target_link));
		return;
	}
	std::atomic_ref<std::uint8_t> replacement{site.next_replacement};
	const auto next = replacement.load(std::memory_order_relaxed);
	gadget_cache::store_pair(&site.entries[next & 1U], target,
		gadget_cache::word_of(target_link));
	replacement.store(static_cast<std::uint8_t>((next + 1U) & 1U),
		std::memory_order_relaxed);
}

TargetLinkTable *Space::target_table(ExecutionMode mode) noexcept
{
	auto &published = targets_[mode_index(mode)];
	if (auto *table = published.load(std::memory_order_acquire))
		return table;
	const std::lock_guard guard{targets_mutex_};
	if (auto *table = published.load(std::memory_order_relaxed))
		return table;
	try {
		auto &owned = targets_storage_[mode_index(mode)];
		owned = std::make_unique<TargetLinkTable>(vacant_target_links());
		account_.charge(static_cast<std::ptrdiff_t>(
			malloc_good_size(sizeof(TargetLinkTable))));
		published.store(owned.get(), std::memory_order_release);
		return owned.get();
	} catch (...) {
		return nullptr;
	}
}

const TargetLinkEntry *Space::target_links(ExecutionMode mode) noexcept
{
	const auto *table = target_table(mode);
	return table == nullptr ? vacant_target_links().data() : table->data();
}

void Space::train_target(ExecutionMode mode, std::uint64_t rip,
	const BlockLink *target) noexcept
{
	auto *table = target_table(mode);
	if (table == nullptr)
		return;
	auto &entry = (*table)[target_link_index(rip)];
	/* Translated code reads the entry, so a shared one is written only
	 * when it changes: the blocks that run announce themselves each time
	 * their slot is used. */
	std::uint64_t held_rip = 0;
	std::uint64_t held_link = 0;
	gadget_cache::load_pair(&entry, held_rip, held_link);
	if (held_rip != rip || held_link != gadget_cache::word_of(target))
		gadget_cache::store_pair(&entry, rip,
			gadget_cache::word_of(target));
}

LinkRecord *Space::activate(LinkRecord *memo, std::uint64_t address,
	ExecutionMode mode, const Body &body, std::uint64_t changes)
{
	auto *record = memo != nullptr && memo->link.guest_rip == address ?
		memo : nullptr;
	/* A record that already names the body has nothing to do but be
	 * announced; the common case, and it needs no lock. */
	if (record == nullptr || !record->names(body)) {
		auto &shard = shard_of(mode, address);
		const std::lock_guard guard{shard.mutex};
		if (record == nullptr)
			record = shard.links[mode_index(mode)].find(address);
		if (record == nullptr)
			return nullptr;
		if (gate_.load(std::memory_order_seq_cst) == changes &&
		    !body.stale.load(std::memory_order_acquire)) {
			bool listed = record->listed;
			if (!listed) {
				try {
					shard.live_links.push_back(record);
					record->listed = true;
					listed = true;
				} catch (...) {
					/* Without a place on the list a reset could
					 * not find the record, so it stays
					 * unlinked. */
				}
			}
			if (listed)
				record->link_to(body);
		}
	}
	train_target(mode, address, &record->link);
	return record;
}

void Space::reset_record_of(Body &body, bool clear_indexed) noexcept
{
	const auto mode = body.program.execution_mode();
	const auto address = body.program.address();
	auto &shard = shard_of(mode, address);
	const std::lock_guard guard{shard.mutex};
	auto *record = shard.links[mode_index(mode)].find(address);
	if (record == nullptr)
		return;
	if (clear_indexed && record->indexed == &body)
		record->indexed = nullptr;
	if (record->names(body))
		record->reset();
}

void Space::note_store(std::uint64_t address, std::size_t size)
{
	if (size == 0)
		return;
	const bool share = shared();
	const auto first = address & page_mask;
	const auto last = (address + size - 1U) & page_mask;
	for (auto page = first;; page += DARWIN_UML_GUEST_PAGE_SIZE) {
		auto &shard = page_shard_of(page);
		{
			const std::lock_guard guard{shard.mutex};
			if (const auto found = shard.pages.find(page);
			    found != shard.pages.end()) {
				/* Where translations are shared, a store makes the
				 * ones it overlaps stale for every executor, and a
				 * build that read the page before it is not kept.
				 * One cache's own revalidates by stamps and
				 * bytes, as it always has. */
				if (share)
					++found->second.stores;
				for (auto *body = found->second.first;
				     body != nullptr;
				     body = body->page_next[page_entry(*body, page)])
					if (source_overlaps(body->program, address,
						    size)) {
						if (share)
							body->stale.store(true,
								std::memory_order_release);
						reset_record_of(*body, share);
					}
			}
		}
		if (page == last)
			break;
	}
}

void Space::reset_all() noexcept
{
	for (auto &shard : shards_) {
		const std::lock_guard guard{shard.mutex};
		for (auto *record : shard.live_links) {
			record->reset();
			record->listed = false;
		}
		shard.live_links.clear();
		for (std::size_t mode = 0; mode < shard.live_indirect.size();
		     ++mode) {
			const auto empty = empty_target(modes[mode]);
			/* The reset cells name this address space's own empty
			 * record, so a site never points at another's link
			 * state. */
			for (auto *site : shard.live_indirect[mode]) {
				for (auto &entry : site->entries)
					gadget_cache::store_pair(&entry, empty,
						gadget_cache::word_of(
							empty_link_[mode]));
				std::atomic_ref<std::uint8_t>{site->next_replacement}
					.store(0, std::memory_order_relaxed);
			}
			shard.live_indirect[mode].clear();
		}
	}
}

void Space::observe(std::uint64_t changes, ProgramCacheCodeChangeReader reader,
	void *context)
{
	if (reset_done_.load(std::memory_order_acquire) >= changes)
		return;
	const std::lock_guard reset{reset_mutex_};
	const auto since = reset_done_.load(std::memory_order_relaxed);
	if (since >= changes)
		return;
	/* Raising the gate first means an activation racing this either lands
	 * before its record is reset or sees the gate and stays unlinked. */
	gate_.store(changes, std::memory_order_seq_cst);
	bool described = false;
	if (reader != nullptr && context != nullptr) {
		std::array<ProgramCodeChange, code_change_replay_limit> ranges;
		std::uint64_t current = 0;
		const auto count = reader(context, since, ranges.data(),
			ranges.size(), &current);
		if (count >= 0) {
			described = true;
			std::uint64_t pages = 0;
			for (int index = 0; index < count; ++index) {
				pages += ranges[index].length /
					DARWIN_UML_GUEST_PAGE_SIZE + 1U;
				if (pages > code_change_replay_pages) {
					described = false;
					break;
				}
			}
			for (int index = 0; described && index < count; ++index) {
				const auto address = ranges[index].address;
				const auto length =
					static_cast<std::size_t>(ranges[index].length);
				if (length == 0)
					continue;
				const auto first = address & page_mask;
				const auto last = (address + length - 1U) & page_mask;
				for (auto page = first;;
				     page += DARWIN_UML_GUEST_PAGE_SIZE) {
					auto &shard = page_shard_of(page);
					{
						const std::lock_guard guard{shard.mutex};
						if (const auto found = shard.pages.find(page);
						    found != shard.pages.end())
							for (auto *body = found->second.first;
							     body != nullptr;
							     body = body->page_next[page_entry(
								     *body, page)])
								if (source_overlaps(body->program,
									    address, length))
									reset_record_of(*body, false);
					}
					if (page == last)
						break;
				}
			}
		}
	}
	if (!described)
		reset_all();
	reset_done_.store(changes, std::memory_order_release);
}

void Space::accept(std::uint64_t changes)
{
	const std::lock_guard reset{reset_mutex_};
	if (reset_done_.load(std::memory_order_relaxed) >= changes)
		return;
	gate_.store(changes, std::memory_order_seq_cst);
	reset_done_.store(changes, std::memory_order_release);
}

std::size_t Space::records() const noexcept
{
	std::size_t count = 0;
	for (const auto &shard : shards_)
		for (std::size_t mode = 0; mode < 2; ++mode)
			count += shard.links[mode].size() +
				shard.indirect[mode].size();
	return count;
}

bool Space::sweep_due() const noexcept
{
	return gadget_cache::sweep_due(records(), kept_,
		departed_.load(std::memory_order_relaxed));
}

std::array<Space::LinkMarks, 2> Space::link_marks() const
{
	std::array<LinkMarks, 2> marks;
	for (std::size_t mode = 0; mode < 2; ++mode) {
		std::array<const LinkTable *, shard_count> tables;
		for (std::size_t index = 0; index < shard_count; ++index)
			tables[index] = &shards_[index].links[mode];
		marks[mode] = LinkTable::marks_of(tables);
		if (const auto *empty =
			    find_link(empty_target(modes[mode]), modes[mode]);
		    empty != nullptr)
			marks[mode].mark(empty);
	}
	return marks;
}

std::array<Space::SiteMarks, 2> Space::site_marks() const
{
	std::array<SiteMarks, 2> marks;
	for (std::size_t mode = 0; mode < 2; ++mode) {
		std::array<const SiteTable *, shard_count> tables;
		for (std::size_t index = 0; index < shard_count; ++index)
			tables[index] = &shards_[index].indirect[mode];
		marks[mode] = SiteTable::marks_of(tables);
	}
	return marks;
}

Space::Released Space::sweep(const std::array<LinkMarks, 2> &links,
	const std::array<SiteMarks, 2> &sites)
{
	assert(!shared());
	/* Everything that allocates comes first. */
	std::array<std::vector<LinkRecord *>, shard_count> live_links;
	std::array<std::array<std::vector<IndirectLink *>, 2>, shard_count>
		live_indirect;
	for (std::size_t index = 0; index < shard_count; ++index) {
		const auto &shard = shards_[index];
		const std::lock_guard guard{shard.mutex};
		live_links[index].reserve(shard.live_links.size());
		for (auto *record : shard.live_links)
			if (links[0].marked(record) || links[1].marked(record))
				live_links[index].push_back(record);
		for (std::size_t mode = 0; mode < 2; ++mode) {
			live_indirect[index][mode].reserve(
				shard.live_indirect[mode].size());
			for (auto *site : shard.live_indirect[mode])
				if (sites[mode].marked(
					    reinterpret_cast<const IndirectRecord *>(
						    reinterpret_cast<std::uintptr_t>(site) -
						    offsetof(IndirectRecord, site))))
					live_indirect[index][mode].push_back(site);
		}
	}

	/* From here each step leaves the address space consistent. A site cell or
	 * target table entry naming a record the sweep releases is emptied. */
	for (std::size_t mode = 0; mode < 2; ++mode) {
		const auto empty = empty_target(modes[mode]);
		const auto clear = [&](auto &entry) {
			if (!links[mode].marked(record_of(entry.link)))
				gadget_cache::store_pair(&entry, empty,
					gadget_cache::word_of(empty_link_[mode]));
		};
		for (auto &shard : shards_) {
			const std::lock_guard guard{shard.mutex};
			shard.indirect[mode].for_each([&](IndirectRecord &record) {
				for (auto &entry : record.site.entries)
					clear(entry);
			});
		}
		if (auto *table = targets_[mode].load(std::memory_order_acquire))
			for (auto &entry : *table)
				if (entry.link != vacant_target_links().front().link)
					clear(entry);
	}
	Released released{};
	for (std::size_t index = 0; index < shard_count; ++index) {
		auto &shard = shards_[index];
		const std::lock_guard guard{shard.mutex};
		shard.live_links = std::move(live_links[index]);
		for (std::size_t mode = 0; mode < 2; ++mode) {
			/* A site whose cells are all empty again is enrolled by its
			 * next training, as one never trained is. */
			const auto empty = empty_target(modes[mode]);
			std::erase_if(live_indirect[index][mode],
				[empty](IndirectLink *site) {
					return untrained(*site, empty);
				});
			shard.live_indirect[mode] =
				std::move(live_indirect[index][mode]);
			released.sites += shard.indirect[mode].sweep(sites[mode]);
			released.links += shard.links[mode].sweep(links[mode]);
		}
	}
	return released;
}

void Space::swept() noexcept
{
	kept_ = records();
	departed_.store(0, std::memory_order_relaxed);
}

#if UML_GADGET_CACHE_CENSUS
Space::LinkBytes Space::link_bytes() const
{
	LinkBytes bytes{};
	for (const auto &shard : shards_) {
		const std::lock_guard guard{shard.mutex};
		for (const auto &mode : shard.links)
			bytes.links += mode.bytes();
		bytes.links += shard.live_links.capacity() * sizeof(LinkRecord *);
		for (const auto &mode : shard.indirect)
			bytes.indirect += mode.bytes();
		for (const auto &live : shard.live_indirect)
			bytes.indirect += live.capacity() * sizeof(IndirectLink *);
	}
	for (const auto &table : targets_)
		if (table.load(std::memory_order_relaxed) != nullptr)
			bytes.targets += sizeof(TargetLinkTable);
	for (const auto &shard : page_shards_) {
		const std::lock_guard guard{shard.mutex};
		bytes.pages += shard.pages.size() *
				(sizeof(std::pair<const std::uint64_t, PageEntry>) +
					2 * sizeof(void *)) +
			shard.pages.bucket_count() * sizeof(void *);
	}
	return bytes;
}

std::size_t Space::link_records() const
{
	std::size_t count = 0;
	for (const auto &shard : shards_) {
		const std::lock_guard guard{shard.mutex};
		for (const auto &mode : shard.links)
			count += mode.size();
	}
	return count;
}

std::size_t Space::stream_index_bytes() const
{
	const std::lock_guard arena{arena_mutex_};
	return streams_.index_bytes() + sources_.index_bytes();
}

std::size_t Space::stream_bytes() const
{
	const std::lock_guard arena{arena_mutex_};
	return streams_.bytes() + sources_.bytes();
}

std::size_t Space::empty_bytes() const
{
	const std::lock_guard arena{arena_mutex_};
	return streams_.empty_bytes() + sources_.empty_bytes();
}
#endif

} // namespace uml::cpu::x86_64::gadgets
