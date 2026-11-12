// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"
#include "../domain_contract.hpp"
#include "../process_singleton.hpp"
#include "address_layout.hpp"
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "bridge_call.hpp"
#include "domain_registry_testing.hpp"
#include "execution_gate.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
using AddressLayout = ConfiguredAddressLayout;

struct BackingKey final {
	std::uint64_t device;
	std::uint64_t inode;

	bool operator==(const BackingKey &) const = default;
};

struct BackingKeyHash final {
	[[nodiscard]] std::size_t operator()(const BackingKey &key) const noexcept
	{
		auto value = key.inode ^ (key.device * 0x9e37'79b9'7f4a'7c15ULL);
		value ^= value >> 33;
		return static_cast<std::size_t>(value);
	}
};

/* One non-executable shared mapping covers the complete UML physical-memory
 * file.  Besides avoiding a host mapping per guest page, this is the
 * invariant that lets a TLB hit become pointer arithmetic.
 */
MappingBatch &mapping_batch()
{
	static thread_local MappingBatch batch;
	return batch;
}

void reset_mapping_batch() noexcept
{
	auto &batch = mapping_batch();
	batch.operations.clear();
	batch.backings.clear();
	batch.address_space = nullptr;
	batch.depth = 0;
	batch.generation = 0;
	batch.invalidation_epoch = 0;
	batch.state.reset();
}

ActiveWalker &active_walker()
{
	static thread_local ActiveWalker active;
	return active;
}

constexpr std::size_t maximum_mapping_history = 4096;

/* The protection bits a mapping keeps. */
constexpr int protection_mask = PROT_READ | PROT_WRITE | PROT_EXEC;

/* What a generation maps at one page, as its fill index word and backing slot
 * say. The backing is the slot's, so the record is good for as long as the
 * caller holds the generation's lock. */
struct PageMapping final {
	MappedBacking *backing;
	std::uint64_t offset;
	int fd;
	int protection;
	bool guest_code;
	bool guest_writable;
};

[[nodiscard]] int protection_of(const FillIndex::Fields &fields) noexcept
{
	return (fields.read ? PROT_READ : 0) | (fields.write ? PROT_WRITE : 0) |
		(fields.exec ? PROT_EXEC : 0);
}

/* The record of a page whose word is `word` (0: none), or nothing when the
 * page is absent. */
[[nodiscard]] std::optional<PageMapping> decode_page(
	const GenerationState &state, std::uint64_t page,
	std::uintptr_t word) noexcept
{
	if (word == 0)
		return std::nullopt;
	const auto fields = FillIndex::decode(word);
	const auto entry = state.backings.entry(fields.slot, page);
	if (entry.backing == nullptr)
		return std::nullopt;
	return PageMapping{entry.backing,
		fields.host - reinterpret_cast<std::uintptr_t>(entry.backing->base()),
		entry.fd, protection_of(fields), fields.guest_code,
		fields.guest_writable};
}

/* What the generation maps at `page`. For a holder of its lock. */
[[nodiscard]] std::optional<PageMapping> find_page(
	const GenerationState &state, std::uint64_t page) noexcept
{
	return decode_page(state, page, state.fill_index.lookup(page));
}

/* Whether a translator can hold code for the page: it fetches only from a
 * guest-code page that grants execute. */
[[nodiscard]] bool can_hold_translation(const PageMapping &mapping) noexcept
{
	return mapping.guest_code && (mapping.protection & PROT_EXEC) != 0;
}

/* Raised once for the process, before the first x86 translation exists. x86
 * code sees every store at once, the kernel's included, so a kernel write into
 * a guest-code page must publish a code change itself. arm64 and AArch32 code
 * see such a write only after their own IC IVAU or cacheflush(), which
 * publishes it, and a process that runs nothing else does not pay for a
 * publication per uaccess. */
std::atomic_bool &coherent_kernel_code_writes()
{
	static std::atomic_bool required{};
	return required;
}

/* Whether a kernel write just copied into a guest-code page must publish.
 * The fence pairs with the one after the flag is raised: either this load
 * sees the flag, or the first x86 translator fetches the bytes just written. */
[[nodiscard]] bool kernel_code_write_publishes() noexcept
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	return coherent_kernel_code_writes().load(std::memory_order_relaxed);
}

/* Whether replacing `before` with `after` can leave a translation of the
 * page wrong. A translation exists only for a page that could hold one, so
 * a page that could not changes no code, whatever it becomes. One that
 * could changes code when it stops being able to or when its bytes may
 * differ, which is another backing or another place in it; a change of
 * protection or of who may write that keeps execute and the bytes changes
 * none. A store into the page publishes for itself (a translation's page is
 * in the code-page registry), and the page's other writers publish with
 * their own stores. */
[[nodiscard]] bool changes_translatable_code(
	const std::optional<PageMapping> &before,
	const std::optional<PageMapping> &after) noexcept
{
	if (!before || !can_hold_translation(*before))
		return false;
	if (!after || !can_hold_translation(*after))
		return true;
	return before->backing != after->backing ||
		before->offset != after->offset;
}

/* The pages of one mapping change whose code changes, as the smallest span
 * covering them, or the whole address space. */
struct CodeSpan final {
	std::uint64_t first{~std::uint64_t{0}};
	std::uint64_t end{};
	bool whole{};

	void add(std::uint64_t page) noexcept
	{
		first = std::min(first, page);
		end = std::max(end, page + page_size);
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return !whole && end <= first;
	}
};

/* Notes that the page at `address`, which was `before` and becomes `after`,
 * changes code, when it does. A page stamp names the bytes a translation was
 * validated against, and the stores that move it are those a translator sees
 * land on a page it holds -- a page mapped without execute, or reached by
 * another alias, is not one. So a page leaving the translatable state moves
 * its own stamp as it is published: whatever later returns to it compares
 * its saved source instead of trusting a stamp that nothing could move. The
 * stamp exists only once something has translated the page, and a page
 * nothing translated has none to move. */
void note_code_change(CodeSpan &code, std::uint64_t address,
	const std::optional<PageMapping> &before,
	const std::optional<PageMapping> &after) noexcept
{
	if (!changes_translatable_code(before, after))
		return;
	code.add(address);
	before->backing->retire_code_page(before->offset);
}

struct MappingChangePlan final {
	std::uint64_t address;
	std::uint64_t length;
	CodeSpan code;
};

class PendingMappingChanges final {
public:
	PendingMappingChanges(GenerationState &state,
		std::span<const MappingChangePlan> changes)
		: state_{state}
	{
		const auto first = state.mapping_changes.load(
			std::memory_order_acquire) + 1;
		try {
			for (std::size_t index = 0; index < changes.size(); ++index) {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::history);
				state.mapping_history.push_back({first + index,
					changes[index].address, changes[index].length});
				++added_;
				const auto &code = changes[index].code;
				if (code.empty())
					continue;
				/* A whole-space change names the range {0, 0}. */
				if (code_changes_ < code_ranges_.size())
					code_ranges_[code_changes_] = code.whole ?
						CodeChangeRange{0, 0} :
						CodeChangeRange{code.first,
							code.end - code.first};
				++code_changes_;
			}
		} catch (...) {
			rollback_history();
			throw;
		}
	}

	~PendingMappingChanges()
	{
		if (!committed_)
			rollback_history();
	}

	PendingMappingChanges(const PendingMappingChanges &) = delete;
	PendingMappingChanges &operator=(const PendingMappingChanges &) = delete;

	void commit() noexcept
	{
		if (added_ != 0)
			state_.mapping_changes.fetch_add(
				added_, std::memory_order_release);
		if (code_changes_ > code_ranges_.size())
			publish_untracked_code_changes(state_, code_changes_);
		else
			publish_code_changes(state_,
				{code_ranges_.data(), code_changes_});
		while (state_.mapping_history.size() > maximum_mapping_history)
			state_.mapping_history.pop_front();
		committed_ = true;
	}

private:
	void rollback_history() noexcept
	{
		while (added_ != 0) {
			state_.mapping_history.pop_back();
			--added_;
		}
	}

	GenerationState &state_;
	/* One batch rarely changes code in more than a few of its ranges; a wider
	 * one is published without a history rather than allocating here, where a
	 * throw would have to unwind a partly recorded mapping change. */
	std::array<CodeChangeRange, 16> code_ranges_{};
	std::size_t added_{};
	std::size_t code_changes_{};
	bool committed_{};
};

[[nodiscard]] bool same_mapping(
	const PageMapping &left, const PageMapping &right) noexcept
{
	return left.backing == right.backing && left.offset == right.offset &&
		left.fd == right.fd && left.protection == right.protection &&
		left.guest_code == right.guest_code &&
		left.guest_writable == right.guest_writable;
}

[[nodiscard]] bool same_mapping(
	const std::optional<PageMapping> &left,
	const std::optional<PageMapping> &right) noexcept
{
	return left.has_value() == right.has_value() &&
		(!left || same_mapping(*left, *right));
}

/* The fill index word of a mapped page that lies in backing slot `slot`: its
 * host address and what the guest may do there, as translate_in() judges it.
 * map() has checked that the address is one a word can hold. */
[[nodiscard]] std::uintptr_t fill_word(
	const PageMapping &mapping, unsigned slot) noexcept
{
	return FillIndex::encode({
		reinterpret_cast<std::uintptr_t>(
			mapping.backing->at(mapping.offset)),
		(mapping.protection & PROT_READ) != 0,
		(mapping.protection & PROT_WRITE) != 0,
		(mapping.protection & PROT_EXEC) != 0,
		mapping.guest_code, mapping.guest_writable, slot});
}

struct PlannedMappingPage final {
	std::uint64_t address;
	std::optional<PageMapping> original;
	std::optional<PageMapping> final;
	/* Who owns `final`'s backing: the operation that maps it. */
	const std::shared_ptr<MappedBacking> *owner;
	/* The backing slot `final` lies in, which the plan claimed. */
	unsigned slot;
};

/* The step of a mapping change that can fail: the nodes, and the overflow
 * room, that every page the change maps will live in. A page's word is
 * committed only once all of them exist, so a change that fails here has
 * changed nothing but the empty nodes it made. */
void reserve_planned_pages(GenerationState &state,
	std::span<const PlannedMappingPage> pages)
{
	for (const auto &page : pages) {
		if (!page.final || same_mapping(page.original, page.final))
			continue;
		UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::reserve);
		if (!state.fill_index.reserve(page.address))
			throw std::bad_alloc{};
		if (page.slot == BackingSlots::overflow)
			state.backings.reserve_overflow(page.address);
	}
}

/* Writes the planned pages' words, counts them in and out of their slots, and
 * frees the leaves a removal emptied. The reserve step made room for all of
 * it, so nothing here allocates or fails. A removal is for the caller to have
 * quiesced execution for. */
void commit_planned_pages(GenerationState &state,
	std::span<const PlannedMappingPage> pages) noexcept
{
	for (const auto &page : pages) {
		if (same_mapping(page.original, page.final))
			continue;
		const auto old_word = state.fill_index.lookup(page.address);
		const bool had = old_word != 0;
		const auto old_slot = FillIndex::decode(old_word).slot;
		if (page.final) {
			state.backings.add(page.slot, page.address, *page.owner,
				page.final->fd);
			state.fill_index.store(page.address,
				fill_word(*page.final, page.slot));
			if (!had)
				++state.mapped_pages;
		} else {
			state.fill_index.store(page.address, 0);
			if (had)
				--state.mapped_pages;
		}
		/* A page that stays in the overflow table was just given its new
		 * entry; dropping the old one would drop that. */
		if (had && !(page.final && old_slot == BackingSlots::overflow &&
			     page.slot == BackingSlots::overflow))
			state.backings.remove(old_slot, page.address);
	}
	auto trimmed = ~std::uint64_t{0};
	for (const auto &page : pages) {
		if (page.final || !page.original ||
		    FillIndex::leaf_of(page.address) == trimmed)
			continue;
		trimmed = FillIndex::leaf_of(page.address);
		state.fill_index.trim(page.address);
	}
}

/* Empties a generation of its pages. */
void clear_mappings(GenerationState &state) noexcept
{
	state.fill_index.clear();
	state.backings.clear();
	state.mapped_pages = 0;
}

/* Releases the backing slots a change claimed, and the overflow room it
 * reserved, once the change is committed or abandoned. */
class SlotSettler final {
public:
	explicit SlotSettler(GenerationState &state) noexcept : state_{state} {}
	~SlotSettler() { state_.backings.settle(); }
	SlotSettler(const SlotSettler &) = delete;
	SlotSettler &operator=(const SlotSettler &) = delete;

private:
	GenerationState &state_;
};

ActiveGeneration &active_thread_generation()
{
	static thread_local ActiveGeneration active;
	return active;
}

class AddressSpace;

std::atomic_uint64_t &domain_retirement_sequence_storage()
{
	static std::atomic_uint64_t sequence{};
	return sequence;
}

std::atomic_uint64_t &lifetime_retirement_epoch()
{
	static std::atomic_uint64_t epoch{};
	return epoch;
}

/* Generations destroyed in this process, in any domain: what a holder of
 * per-generation state loads to learn that some generation may have died
 * since it last asked generation_live(). Every executor loads it at each
 * entry into translated code, so it has a cache line of its own: a neighbour
 * another thread writes would make each of those loads a miss. */
std::atomic_uint64_t &generation_retirement_sequence()
{
	struct alignas(128) Sequence final {
		std::atomic_uint64_t value{};
	};
	static Sequence sequence;
	return sequence.value;
}

struct ResolvedAddressSpace final {
	std::uint64_t domain{};
	std::uint64_t retirement_sequence{};
	std::shared_ptr<AddressSpace> owner;
};

/* Everything address_space() needs from its thread, behind one thread-local
 * lookup: every resolution is a TLB miss or a mapping call on a vCPU thread.
 *
 * pruned_epoch is the lifetime-retirement epoch at which this thread last
 * found none of its bindings retired. AddressSpace::retire() is the only
 * writer of the lifetime flag that pruning tests, and it advances the epoch
 * after setting it, so a thread that reads an unchanged epoch has nothing to
 * prune.
 *
 * borrowed_accesses counts the AddressSpaceAccess objects on this thread that
 * borrow resolved.owner instead of holding their own reference. While any is
 * alive, resolution does not replace resolved, so the borrowed space outlives
 * every access to it without a shared reference-count round trip, which is
 * contended by every vCPU of one guest process. */
struct ThreadAddressSpaceCache final {
	ResolvedAddressSpace resolved;
	std::uint64_t pruned_epoch{~std::uint64_t{0}};
	unsigned borrowed_accesses{};
};

ThreadAddressSpaceCache &thread_address_space_cache()
{
	static thread_local ThreadAddressSpaceCache cache;
	return cache;
}

void prune_retired_thread_state(ThreadAddressSpaceCache &cache) noexcept
{
	const auto epoch =
		lifetime_retirement_epoch().load(std::memory_order_acquire);
	if (cache.pruned_epoch == epoch)
		return;
	const auto domain_retired = [](const auto &state) {
		return state != nullptr && state->address_space_lifetime &&
			state->address_space_lifetime->retired.load(
				std::memory_order_acquire);
	};
	auto &walker = active_walker();
	if (domain_retired(walker.state.get())) {
		walker.state.reset();
		walker.address_space = nullptr;
		walker.generation = 0;
	}
	auto &batch = mapping_batch();
	if (domain_retired(batch.state.get()))
		reset_mapping_batch();
	auto &active = active_thread_generation();
	if (domain_retired(active.state.get())) {
		active.address_space = nullptr;
		active.generation = 0;
		active.state.reset();
	}
	cache.pruned_epoch = epoch;
}

class AddressSpaceAccess;

/* Retirement has to wait for in-flight operations, and it used to do that with a
 * shared_mutex lease taken by every AddressSpaceAccess and held for the whole
 * call. That lock is per address space, so all ten vCPU threads of one guest
 * process serialize on it - libc++'s lock_shared takes a real internal
 * std::mutex - and G120 measured removing it as worth about 31% of the
 * miss-heavy probe, more than the process-global registry lock G101 removed.
 *
 * The replacement is the pattern jit_publication.hpp already uses for
 * jit_store_participants: each thread publishes the address space it is
 * operating on into its own slot, and teardown scans the slots instead of
 * excluding everyone.
 *
 * This is a store-buffer problem and the fences are the design, not a detail.
 * An access stores its slot and then reads `retired_`; retirement stores
 * `retired_` and then reads the slots. Both sides are seq_cst, so the two
 * stores are totally ordered and at least one side observes the other: an
 * access that is not seen by the scan is guaranteed to see the retirement and
 * back out. Relaxing either side reintroduces exactly the window the lease
 * closed. */
struct ActiveAccessSlot final {
	std::atomic<const void *> space{};
};

/* Never destroyed, under the exit contract in docs/architecture.md: a thread
 * can leave its slot, and a domain can retire, after exit() has begun
 * destroying static storage. Both are constant-initialized. */
std::mutex &active_access_registry_mutex()
{
	[[clang::no_destroy]] static std::mutex mutex;
	return mutex;
}

std::vector<ActiveAccessSlot *> &active_access_registry()
{
	[[clang::no_destroy]] static std::vector<ActiveAccessSlot *> slots;
	return slots;
}

ActiveAccessSlot &active_access_slot()
{
	/* Registered on first use and unregistered at thread exit. The scan below
	 * holds the registry mutex, and a thread in the middle of an access clears
	 * its slot from its destructor without needing that mutex, so the scan
	 * always makes progress even while an exiting thread waits to unregister. */
	struct Registration final {
		Registration()
		{
			const std::scoped_lock lock{active_access_registry_mutex()};
			active_access_registry().push_back(&slot);
		}

		~Registration()
		{
			const std::scoped_lock lock{active_access_registry_mutex()};
			auto &slots = active_access_registry();
			slots.erase(std::remove(slots.begin(), slots.end(), &slot),
				slots.end());
		}

		ActiveAccessSlot slot;
	};
	static thread_local Registration registration;
	return registration.slot;
}

void wait_for_active_accesses(const void *space) noexcept
{
	const std::scoped_lock lock{active_access_registry_mutex()};
	for (auto *slot : active_access_registry())
		while (slot->space.load(std::memory_order_seq_cst) == space)
			std::this_thread::yield();
}

class AddressSpace final {
public:
	int map(std::uint64_t generation, std::uint64_t address, int fd,
		std::uint64_t offset,
		std::uint64_t length, int protection, bool guest_code,
		bool guest_writable)
	{
		if ((address | offset | length) % page_size != 0 || length == 0 ||
		    !stays_in_generation(address, length))
			return -1;
		auto backing = backing_for(fd);
		if (!backing || offset > backing->size() ||
		    length > backing->size() - offset)
			return -1;
		/* A page's word holds the host address beside its flags. */
		if (!FillIndex::representable(
			    reinterpret_cast<std::uintptr_t>(backing->at(offset))))
			return -1;
		protection &= protection_mask;
		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this &&
		    batch.generation == generation) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch);
			batch.operations.push_back({
				true, address, length,
				Mapping{std::move(backing), offset, fd, protection,
					guest_code, guest_writable}});
			return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		map_locked(*state, address, length,
			Mapping{std::move(backing), offset, fd, protection,
				guest_code, guest_writable});
		return 0;
	}

	int protect(std::uint64_t address, std::uint64_t length, int protection)
	{
		if ((address | length) % page_size != 0 || length == 0 ||
		    !stays_in_generation(address, length))
			return -1;
		const auto &active = active_thread_generation();
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		protection &= protection_mask;
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		bool changed = false;
		CodeSpan code;
		(void)state->fill_index.scan(address, address + length,
			[&](std::uint64_t page, std::uintptr_t word) {
				const auto fields = FillIndex::decode(word);
				if (protection_of(fields) == protection)
					return false;
				changed = true;
				/* The backing and the bytes stay; only losing execute
				 * leaves a translation wrong. */
				if (fields.guest_code && fields.exec &&
				    (protection & PROT_EXEC) == 0) {
					code.add(page);
					if (const auto mapping = decode_page(
						    *state, page, word))
						mapping->backing->retire_code_page(
							mapping->offset);
				}
				return false;
			});
		if (!changed)
			return 0;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		PendingMappingChanges pending{*state, changes};
		(void)state->fill_index.scan(address, address + length,
			[&](std::uint64_t page, std::uintptr_t word) {
				auto fields = FillIndex::decode(word);
				fields.read = (protection & PROT_READ) != 0;
				fields.write = (protection & PROT_WRITE) != 0;
				fields.exec = (protection & PROT_EXEC) != 0;
				state->fill_index.store(page, FillIndex::encode(fields));
				return false;
			});
		pending.commit();
		return 0;
	}

	int unmap(std::uint64_t generation, std::uint64_t address,
		  std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return -1;
		const auto end = address + length;
		if (end > std::numeric_limits<std::uint64_t>::max() -
				(page_size - 1))
			return -1;
		const auto first = address & ~(page_size - 1);
		const auto rounded_end = (end + page_size - 1) & ~(page_size - 1);
		if (AddressLayout::classify(first, rounded_end - first) ==
		    AddressDomain::invalid)
			return -1;
		address = first;
		length = rounded_end - first;
		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this &&
		    batch.generation == generation) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch);
			batch.operations.push_back(
				{false, address, length, Mapping{}});
			return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		/* Linux invalidates after it changes a PTE and before it frees the
		 * page.  A walker which sampled the old PTE may not have published it
		 * yet, so the range can be absent here and the snapshot still stale:
		 * reject every batch that began before this point, whether or not a
		 * mapping was removed. */
		(void)unmap_locked(*state, address, length);
		state->invalidation_epoch.fetch_add(1, std::memory_order_release);
		return 0;
	}

	int flush_generation(std::uint64_t generation)
	{
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		if (state->mapped_pages == 0) {
			/* A batch which started before this flush must not publish an old
			 * page-table snapshot after the flush returns, even when no mapping
			 * had reached the host cache yet. */
			state->invalidation_epoch.fetch_add(
				1, std::memory_order_release);
			return 0;
		}
		/* A zero-length history entry tells every executor to discard its
		 * complete private TLB.  Whole-mm Linux invalidation cannot retain a
		 * narrower range, and clearing the generation here replaces one host
		 * execution quiesce per VMA with one generation-wide boundary. */
		const std::array changes{
			MappingChangePlan{0, 0, {.whole = true}}};
		PendingMappingChanges pending{*state, changes};
		state->invalidation_epoch.fetch_add(
			1, std::memory_order_release);
		clear_mappings(*state);
		pending.commit();
		return 0;
	}

	int activate(std::uint64_t generation)
	{
		/* A UML process is a Darwin thread, so the selected Linux mm is a
		 * thread property. A process-global selector lets a sibling switch mm
		 * between activate() and map(), installing a page in the wrong guest.
		 * Translation already carries an explicit generation; activation only
		 * supplies the implicit generation used by mapping-management calls.
		 */
		auto &active = active_thread_generation();
		if (active.address_space == this && active.generation == generation &&
		    active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return 0;
		active.address_space = this;
		active.generation = generation;
		active.state = state_for(generation);
		return 0;
	}

	/* Whether the generation still has an address space: destroying one
	 * removes its state, and a generation never comes back. */
	int generation_live(std::uint64_t generation)
	{
		if (generation == 0)
			return 1;
		std::shared_lock lock{states_mutex_};
		const auto found = states_.find(generation);
		return found != states_.end() &&
			!found->second->retired.load(std::memory_order_acquire);
	}

	int destroy(std::uint64_t generation)
	{
		if (generation == 0)
			return 0;
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		{
			std::lock_guard lock{state->mappings_mutex};
			const std::array changes{
				MappingChangePlan{0, 0, {.whole = true}}};
			PendingMappingChanges pending{*state, changes};
			clear_mappings(*state);
			pending.commit();
		}
		state->retired.store(true, std::memory_order_release);
		{
			std::lock_guard lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end() && found->second == state)
				states_.erase(found);
		}
		/* After the erasure, so a holder that loads the new count and then
		 * asks generation_live() is told the generation is gone. */
		generation_retirement_sequence().fetch_add(
			1, std::memory_order_release);
		auto &active = active_thread_generation();
		if (active.address_space == this && active.generation == generation) {
			active.state.reset();
			active.address_space = nullptr;
		}
		return 0;
	}

	void retire() noexcept
	{
		if (retired_.exchange(true, std::memory_order_seq_cst))
			return;
		/* Ordered against the slot store in AddressSpaceAccess: an access this
		 * scan does not observe is guaranteed to observe the retirement. */
		wait_for_active_accesses(this);
		lifetime_->retired.store(true, std::memory_order_release);
		lifetime_retirement_epoch().fetch_add(1, std::memory_order_acq_rel);
		{
			std::lock_guard lock{states_mutex_};
			for (auto &[generation, state] : states_) {
				(void)generation;
				state->retired.store(true, std::memory_order_release);
			}
			states_.clear();
		}
		generation_retirement_sequence().fetch_add(
			1, std::memory_order_release);
		auto &active = active_thread_generation();
		if (active.address_space == this) {
			active.address_space = nullptr;
			active.generation = 0;
			active.state.reset();
		}
		auto &batch = mapping_batch();
		if (batch.address_space == this) {
			batch.operations.clear();
			batch.backings.clear();
			batch.address_space = nullptr;
			batch.depth = 0;
			batch.generation = 0;
			batch.invalidation_epoch = 0;
			batch.state.reset();
		}
		auto &walker = active_walker();
		if (walker.address_space == this && walker.state) {
			walker.state.reset();
			walker.address_space = nullptr;
			walker.generation = 0;
		}
	}

	int discard_if_stale(std::uint64_t address, int fd, std::uint64_t offset)
	{
		address &= ~(page_size - 1);
		const auto &active = active_thread_generation();
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		const auto names_other_backing = [&](const GenerationState &state) {
			const auto mapping = find_page(state, address);
			return mapping && (mapping->fd != fd || mapping->offset != offset);
		};
		{
			/* Every kernel uaccess asks, and nearly every one finds the page
			 * absent or already right; neither is worth stopping the
			 * generation's executors for.  The active generation is
			 * borrowed without a reference, as a translation borrows it.
			 * The caller's backing is only compared, never installed, so it
			 * is never looked up. */
			auto reference = reference_for(generation);
			std::shared_lock lock{reference.state->mappings_mutex};
			if (!names_other_backing(*reference.state))
				return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		if (!names_other_backing(*state))
			return 0;
		/* The caller pinned this backing from a PTE it no longer holds the
		 * lock of, so the disagreement says only that one of the two is
		 * stale, not which.  Installing the caller's page here is how a
		 * uaccess that raced a copy-on-write pointed every sibling back at
		 * the page Linux had just replaced, writable, and their stores were
		 * lost to it.  An absent page is always sound: the next access
		 * faults and a walker replays the PTE under the invalidation epoch.
		 * This is an invalidation like any other, so older batches go too. */
		(void)unmap_locked(*state, address, page_size);
		state->invalidation_epoch.fetch_add(1, std::memory_order_release);
		return 0;
	}

	/* A kernel write (`kernel_write`) publishes a code change only when
	 * kernel_code_write_publishes() says x86 code may hold the page. */
	int copy_from_buffer(std::uint64_t address, const void *buffer,
			     std::uint64_t length, bool kernel_write)
	{
		if (length == 0)
			return 0;
		if (buffer == nullptr || address + length < address ||
		    length > page_size - (address & (page_size - 1)))
			return -1;
		const auto page = address & ~(page_size - 1);
		const auto in_page = address & (page_size - 1);
		/* The mapping must remain stable while the written range is copied,
		 * but neither the mapping table nor generation metadata is structurally
		 * changed.  Let unrelated page copies and translations proceed in
		 * parallel.  Copying only the bytes changed by uaccess is essential:
		 * a sibling guest thread may update another byte in the same guest page
		 * through its Darwin 16 KiB host-page shadow.
		 */
		const auto &active = active_thread_generation();
		const auto generation = generation_for(page,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		auto reference = reference_for(generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto word = reference.state->fill_index.lookup(page);
		if (word == 0)
			return 0;
		std::memcpy(reinterpret_cast<void *>(
				    (word & FillIndex::address_mask) + in_page),
			buffer, length);
		if ((word & FillIndex::guest_code_page) != 0 &&
		    (!kernel_write || kernel_code_write_publishes())) {
			if (const auto mapping = decode_page(
				    *reference.state, page, word))
				MappedBacking::invalidate_code_page(
					mapping->backing->code_page_handle(mapping->offset));
			publish_code_change(*reference.state, page);
		}
		return 0;
	}

	int translate(std::uint64_t generation, std::uint64_t address,
		      darwin_uml_static_access access, void **result)
	{
		if (result == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*result = nullptr;
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		/* The execution gate excludes mapping mutation in this generation.
		 * Keep each generation's mapping lookup independent so unrelated guest
		 * processes neither share a hash table nor touch the same lock cacheline.
		 */
		auto reference = reference_for(resolved_generation);
		return translate_in(*reference.state, address, access, result);
	}

	/* translate() for an entered execution context of `generation`: the
	 * context holds the generation's state, so a guest address needs neither
	 * the thread's address space nor its active generation. Anything else
	 * answers execution_context_fallback for the general path. */
	static constexpr int execution_context_fallback = -1000;
	[[nodiscard]] static int execution_context_translate(
		ExecutionContext *context, std::uint64_t generation,
		std::uint64_t address, darwin_uml_static_access access,
		void **result)
	{
		if (context == nullptr || !context->entered ||
		    context->generation != generation || result == nullptr)
			return execution_context_fallback;
		const auto page = address & ~(page_size - 1);
		if (!AddressLayout::is_guest(page) ||
		    context->state->retired.load(std::memory_order_acquire))
			return execution_context_fallback;
		*result = nullptr;
		const auto wanted = access == DARWIN_UML_STATIC_EXECUTE ?
			FillIndex::executable :
			access == DARWIN_UML_STATIC_WRITE ?
				FillIndex::writable : FillIndex::readable;
		const auto entry = context->state->fill_index.find(page);
		if ((entry & wanted) != 0) {
			*result = reinterpret_cast<void *>(
				(entry & FillIndex::address_mask) +
				(address & (page_size - 1)));
			return DARWIN_UML_STATIC_TRANSLATED;
		}
		return translate_in(*context->state, address, access, result);
	}

	[[nodiscard]] static int translate_in(GenerationState &state,
		std::uint64_t address, darwin_uml_static_access access,
		void **result)
	{
		const auto page = address & ~(page_size - 1);
		std::shared_lock lock{state.mappings_mutex};
		const auto word = state.fill_index.lookup(page);
		if (word == 0)
			return DARWIN_UML_STATIC_UNMAPPED;
		const auto wanted = access == DARWIN_UML_STATIC_EXECUTE ?
			FillIndex::executable :
			access == DARWIN_UML_STATIC_WRITE ?
				FillIndex::writable : FillIndex::readable;
		if ((word & wanted) == 0)
			return DARWIN_UML_STATIC_PERMISSION;
		*result = reinterpret_cast<void *>(
			(word & FillIndex::address_mask) + (address & (page_size - 1)));
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	int code_page(std::uint64_t generation, std::uint64_t address,
		void **handle, darwin_uml_static_code_page_stamp *stamp)
	{
		if (handle == nullptr || stamp == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*handle = nullptr;
		*stamp = {};
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		auto reference = reference_for(resolved_generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto mapping = find_page(*reference.state, page);
		if (!mapping)
			return DARWIN_UML_STATIC_UNMAPPED;
		if (!can_hold_translation(*mapping))
			return DARWIN_UML_STATIC_PERMISSION;
		auto *code_page = mapping->backing->code_page_handle(mapping->offset);
		if (code_page == nullptr ||
		    !MappedBacking::load_code_page_stamp(
			    code_page, stamp->identity, stamp->version))
			return DARWIN_UML_STATIC_HOST_ERROR;
		*handle = code_page;
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	[[nodiscard]] std::uint64_t changes(std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		/* The active generation owns this state for the lifetime of the
		 * thread's activation.  Read its counters in place: copying the
		 * shared_ptr here made every translated block contend on the same
		 * reference count in multi-process workloads.
		 */
		if (active.address_space == this &&
		    active.generation == generation && active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return active.state->mapping_changes.load(
				std::memory_order_acquire);
		return state_for(generation)->mapping_changes.load(
			std::memory_order_acquire);
	}

	int mapping_changes(std::uint64_t generation, std::uint64_t sequence,
		struct darwin_uml_static_mapping_change *output,
		std::size_t capacity, std::uint64_t *current_sequence)
	{
		if (current_sequence == nullptr || (capacity != 0 && output == nullptr))
			return -1;
		/* A vCPU asks after every mapping change it notices, so this runs
		 * as often as the misses do: resolve the state the way translate()
		 * does, through the thread's active generation, not through the
		 * address space's state table and its lock. */
		auto reference = reference_for(generation);
		auto *state = reference.state;
		std::shared_lock lock{state->mappings_mutex};
		const auto current = state->mapping_changes.load(
			std::memory_order_acquire);
		*current_sequence = current;
		if (sequence >= current)
			return 0;
		if (state->mapping_history.empty() ||
		    sequence + 1 < state->mapping_history.front().sequence)
			return -1;
		const auto first_sequence =
			state->mapping_history.front().sequence;
		const auto first_index = sequence < first_sequence ? 0 :
			static_cast<std::size_t>(sequence - first_sequence + 1);
		if (state->mapping_history.size() - first_index > capacity)
			return -1;
		std::size_t count = 0;
		for (auto index = first_index;
		     index < state->mapping_history.size(); ++index) {
			const auto &change = state->mapping_history[index];
			if (change.length == 0)
				return -1;
			output[count++] = {change.address, change.length};
		}
		return static_cast<int>(count);
	}

	void invalidate_code(std::uint64_t address)
	{
		const auto &active = active_thread_generation();
		/* Translated execution normally publishes through its cached context.
		 * Keep this routed fallback coherent with the same physical-page stamp,
		 * including callers which invalidate code outside translated execution.
		 */
		if (AddressLayout::is_guest(address) &&
		    active.address_space == this && active.state &&
		    !active.state->retired.load(std::memory_order_acquire)) {
			invalidate_code_page(*active.state, address);
			publish_code_change(*active.state, address);
			return;
		}
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		auto state = state_for(generation);
		invalidate_code_page(*state, address);
		publish_code_change(*state, address);
	}

	/* Linux wrote a user page through its own mapping of the frame -- a
	 * ptrace, /proc/pid/mem or process_vm_writev() store into another
	 * process's memory -- so no translated store saw it. Move each code
	 * page's stamp and publish the range in that process's generation, as a
	 * guest code synchronization would. A page mapped without execute
	 * permission holds no translation, which keeps the data transfers of
	 * process_vm_writev() from publishing anything. */
	int invalidate_code(std::uint64_t generation, std::uint64_t address,
			    std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return -1;
		const auto end = address + length;
		if (end > std::numeric_limits<std::uint64_t>::max() -
				(page_size - 1))
			return -1;
		const auto first = address & ~(page_size - 1);
		const auto rounded_end = (end + page_size - 1) & ~(page_size - 1);
		if (AddressLayout::classify(first, rounded_end - first) ==
		    AddressDomain::invalid)
			return -1;
		auto state = state_for(generation_for(first, generation));
		std::uint64_t publish_first = rounded_end;
		std::uint64_t publish_end = first;
		{
			std::shared_lock lock{state->mappings_mutex};
			for (auto page = first; page < rounded_end;
			     page += page_size) {
				const auto mapping = find_page(*state, page);
				if (mapping) {
					if (!mapping->guest_code)
						continue;
					MappedBacking::invalidate_code_page(
						mapping->backing->code_page_handle(
							mapping->offset));
				}
				publish_first = std::min(publish_first, page);
				publish_end = page + page_size;
			}
		}
		if (publish_first < publish_end)
			::uml::memory::publish_code_change(*state, publish_first,
				publish_end - publish_first);
		return 0;
	}

	[[nodiscard]] std::uint64_t active_generation() const noexcept
	{
		const auto &active = active_thread_generation();
		return active.address_space == this && active.state &&
			!active.state->retired.load(std::memory_order_acquire) ?
			active.generation : 0;
	}

	void forget_descriptor(int fd)
	{
		std::lock_guard lock{backing_mutex_};
		descriptor_backings_.erase(fd);
	}

	int begin_batch(std::uint64_t generation)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0) {
			if (batch.address_space != this ||
			    batch.generation != generation)
				return -1;
			++batch.depth;
			return 0;
		}
		/* A walker has already resolved the generation it is bound to. */
		const auto &walker = active_walker();
		auto state = walker.state && walker.address_space == this &&
				walker.generation == generation &&
				!walker.state->retired.load(std::memory_order_acquire) ?
			walker.state : state_for(generation);
		const auto invalidation_epoch = state->invalidation_epoch.load(
			std::memory_order_acquire);
		reset_mapping_batch();
		batch.address_space = this;
		batch.generation = generation;
		batch.invalidation_epoch = invalidation_epoch;
		batch.state = std::move(state);
		batch.depth = 1;
		return 0;
	}

	int end_batch()
	{
		auto &batch = mapping_batch();
		if (batch.depth == 0 || batch.address_space != this)
			return -1;
		if (--batch.depth != 0)
			return 0;
		struct BatchReset final {
			~BatchReset() { reset_mapping_batch(); }
		};
		[[maybe_unused]] BatchReset reset;

		int result = 0;
		auto state = batch.state;
		if (!state || state->retired.load(std::memory_order_acquire))
			return -EAGAIN;
		{
			std::unique_lock lock{state->mappings_mutex};
			if (state->retired.load(std::memory_order_acquire) ||
			    state->invalidation_epoch.load(std::memory_order_acquire) !=
			    batch.invalidation_epoch) {
				result = -EAGAIN;
			} else if (!batch_changes_published_mapping(
					   *state, batch.operations)) {
				/*
				 * Publishing a page which was previously absent cannot
				 * invalidate a running vCPU's positive-only TLB.  Its
				 * mapping miss either finishes before this lock is acquired
				 * or observes the new entry after the lock is released.
				 * Avoid stopping every sibling vCPU for ordinary first-touch
				 * demand paging.
				 */
				apply_batch_locked(*state, batch.operations);
			} else {
				lock.unlock();
				ExecutionQuiesce quiesce{state, batch.generation};
				lock.lock();
				if (state->retired.load(std::memory_order_acquire) ||
				    state->invalidation_epoch.load(
					    std::memory_order_acquire) !=
				    batch.invalidation_epoch) {
					result = -EAGAIN;
				} else {
					apply_batch_locked(*state, batch.operations);
				}
				lock.unlock();
			}
		}
		return result;
	}

	int lock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (active.state)
			return -1;
		active.state = state_for(generation);
		active.address_space = this;
		active.generation = generation;
		return 0;
	}

	int unlock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (!active.state || active.address_space != this ||
		    active.generation != generation)
			return -1;
		active.state.reset();
		active.address_space = nullptr;
		active.generation = 0;
		return 0;
	}

	ExecutionContext *create_execution_context(std::uint64_t generation)
	{
		auto state = state_for(generation);
		return new (std::nothrow) ExecutionContext{
			std::move(state), generation, 0, false};
	}

	static void destroy_execution_context(ExecutionContext *context)
	{
		if (context == nullptr)
			return;
		if (context->entered)
			execution_reader_end(*context->state);
		delete context;
	}

	static int execution_context_begin(ExecutionContext *context,
					   std::uint64_t guest_pc)
	{
		if (context == nullptr || context->entered ||
		    context->state->retired.load(std::memory_order_acquire))
			return -1;
		/* Capture before entering the gate.  If a writer closes the gate in
		 * between, this reader either waits for it or observes its interrupt;
		 * it can never miss a shootdown that is waiting on this reader.
		 */
		context->execution_interrupts = context->state->execution_interrupts.load(
			std::memory_order_acquire);
		if (!execution_reader_begin(*context->state, context->generation,
					    guest_pc))
			return -1;
		context->entered = true;
		return 0;
	}

	static int execution_context_end(ExecutionContext *context)
	{
		if (context == nullptr || !context->entered)
			return -1;
		execution_reader_end(*context->state);
		context->entered = false;
		return 0;
	}

	[[nodiscard]] static const std::uint64_t *execution_context_code_sequence(
		const ExecutionContext *context) noexcept
	{
		static_assert(sizeof(std::atomic_uint64_t) == sizeof(std::uint64_t) &&
			std::atomic_uint64_t::is_always_lock_free,
			"translated code loads the sequence as a plain word");
		if (context == nullptr)
			return nullptr;
		return reinterpret_cast<const std::uint64_t *>(
			&context->state->code_changes);
	}

	static darwin_uml_static_sequences execution_context_sequences(
		const ExecutionContext *context)
	{
		if (context == nullptr)
			return {};
		return {
			context->state->mapping_changes.load(std::memory_order_acquire),
			context->state->code_changes.load(std::memory_order_acquire),
			context->state->execution_interrupts.load(
				std::memory_order_acquire) !=
				context->execution_interrupts,
		};
	}

private:
	friend class AddressSpaceAccess;

	[[nodiscard]] static bool stays_in_generation(
		std::uint64_t address, std::uint64_t length) noexcept
	{
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return false;
		return AddressLayout::classify(address, length) !=
			AddressDomain::invalid;
	}

	static std::uint64_t generation_for(std::uint64_t address,
					    std::uint64_t generation) noexcept
	{
		return AddressLayout::is_guest(address) ? generation : 0;
	}

	GenerationReference reference_for(std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		if (active.address_space == this &&
		    active.generation == generation && active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return {{}, active.state.get()};
		auto owner = state_for(generation);
		auto *state = owner.get();
		return {std::move(owner), state};
	}

	std::shared_ptr<MappedBacking> backing_for(int fd)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this) {
			if (auto found = batch.backings.find(fd);
			    found != batch.backings.end())
				return found->second;
		}
		{
			/* An expired entry stays until the slow path below replaces
			 * it; erasing it here would need the lock exclusively. */
			std::shared_lock lock{backing_mutex_};
			if (auto found = descriptor_backings_.find(fd);
			    found != descriptor_backings_.end()) {
				if (auto backing = found->second.lock())
					return backing;
			}
		}
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return {};
		const BackingKey key{static_cast<std::uint64_t>(status.st_dev),
				     static_cast<std::uint64_t>(status.st_ino)};
		/* Darwin POSIX shared-memory descriptors report st_dev == 0 and
		 * st_ino == 0 for every distinct object.  Such an inode cannot identify
		 * a backing across descriptors: Venus, for example, exports its ring and
		 * reply buffers as separate shm objects with that same stat tuple.  Keep
		 * descriptor-local reuse, but never merge an inode-less object with a
		 * different descriptor. */
		const bool stable_identity = key.inode != 0;
		std::lock_guard lock{backing_mutex_};
		if (stable_identity) {
			if (auto found = backings_.find(key); found != backings_.end()) {
				if (auto backing = found->second.lock()) {
					descriptor_backings_.insert_or_assign(fd, backing);
					if (batch.depth != 0 && batch.address_space == this)
						batch.backings.insert_or_assign(fd, backing);
					return backing;
				}
			}
		}
		auto backing = MappedBacking::create(fd, status);
		if (backing) {
			if (stable_identity)
				backings_.insert_or_assign(key, backing);
			descriptor_backings_.insert_or_assign(fd, backing);
			if (batch.depth != 0 && batch.address_space == this)
				batch.backings.insert_or_assign(fd, backing);
		}
		return backing;
	}

	std::shared_ptr<GenerationState> state_for_locked(
		std::uint64_t generation)
	{
		if (const auto found = states_.find(generation);
		    found != states_.end()) {
			if (!found->second->retired.load(std::memory_order_acquire))
				return found->second;
			states_.erase(found);
		}
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::generation);
		auto state = std::make_shared<GenerationState>(heap_);
		state->address_space_lifetime = lifetime_;
		states_.emplace(generation, state);
		return state;
	}

	std::shared_ptr<GenerationState> state_for(std::uint64_t generation)
	{
		{
			std::shared_lock lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end() &&
			    !found->second->retired.load(std::memory_order_acquire))
				return found->second;
		}
		std::lock_guard lock{states_mutex_};
		return state_for_locked(generation);
	}

	void apply_batch_locked(GenerationState &state,
		std::span<const MappingOperation> operations)
	{
		constexpr std::size_t maximum_plan_reserve = 64 * 1024;
		const auto add_reserve_pages = [&](std::size_t &total,
						 const std::uint64_t pages) {
			if (pages >= maximum_plan_reserve - total)
				total = maximum_plan_reserve;
			else
				total += static_cast<std::size_t>(pages);
		};
		SlotSettler settler{state};
		std::size_t touched_pages = 0;
		std::size_t possible_pages = std::min(
			state.mapped_pages, maximum_plan_reserve);
		for (const auto &operation : operations) {
			const auto operation_pages = operation.length / page_size;
			add_reserve_pages(touched_pages, operation_pages);
			if (operation.map)
				add_reserve_pages(possible_pages, operation_pages);
		}
		const auto plan_reserve = std::min(touched_pages, possible_pages);
		/* The plan lives in the thread's scratch: it is dropped whole when
		 * the batch ends, instead of leaving its largest vector in the
		 * default malloc zone. */
		ScratchScope scratch;
		const ScratchAllocator<char> scratch_allocator{scratch.arena()};
		std::vector<PlannedMappingPage, ScratchAllocator<PlannedMappingPage>>
			pages{scratch_allocator};
		std::unordered_map<std::uint64_t, std::size_t, GuestPageHash,
			std::equal_to<std::uint64_t>,
			ScratchAllocator<std::pair<const std::uint64_t, std::size_t>>>
			page_indices{scratch_allocator};
		std::vector<MappingChangePlan, ScratchAllocator<MappingChangePlan>>
			changes{scratch_allocator};
		/* The operation count is a poor capacity estimate: one coalesced TLB
		 * range can cover thousands of pages. Reserve a checked upper bound on
		 * planned pages, capped to avoid a hostile overlapping batch forcing an
		 * otherwise unnecessary address-space-sized allocation. */
		pages.reserve(plan_reserve);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::batch_index);
		page_indices.reserve(plan_reserve);
		changes.reserve(operations.size());
		const auto page_for = [&](const std::uint64_t address)
			-> PlannedMappingPage & {
			if (const auto found = page_indices.find(address);
			    found != page_indices.end())
				return pages[found->second];
			const auto original = find_page(state, address);
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch_index);
			page_indices.emplace(address, pages.size());
			pages.push_back({address, original, original, nullptr,
				BackingSlots::overflow});
			return pages.back();
		};

		for (const auto &operation : operations) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch_commit);
			bool changed = false;
			/* A translation can exist only for what the page held before
			 * the batch: the vCPUs see the batch whole, so a page it maps
			 * and unmaps again leaves none to retire. A page an earlier
			 * operation of the batch already changed is judged again
			 * against its original, which at worst names it twice. */
			CodeSpan code;
			/* The slot of the backing this operation maps, claimed when the
			 * first page needs it. */
			std::optional<unsigned> operation_slot;
			const auto clear_page = [&](PlannedMappingPage &page) {
				if (!page.final)
					return;
				changed = true;
				note_code_change(code, page.address, page.original,
					std::nullopt);
				page.final.reset();
			};
			const auto operation_pages = operation.length / page_size;
			const auto candidate_pages =
				state.mapped_pages >
					std::numeric_limits<std::size_t>::max() - pages.size() ?
				std::numeric_limits<std::size_t>::max() :
				state.mapped_pages + pages.size();
			if (!operation.map && operation_pages > candidate_pages) {
				const auto end = operation.address + operation.length;
				/* A broad unmap is commonly a sparse exit_mmap() range. It can
				 * only change pages which were already published or mapped by an
				 * earlier operation in this batch; do not materialize every hole. */
				(void)state.fill_index.scan(operation.address, end,
					[&](std::uint64_t address, std::uintptr_t) {
						clear_page(page_for(address));
						return false;
					});
				for (auto &page : pages)
					if (page.address >= operation.address &&
					    page.address < end)
						clear_page(page);
			} else {
				for (std::uint64_t position = 0;
				     position < operation.length;
				     position += page_size) {
					const auto page_address =
						operation.address + position;
					if (!operation.map) {
						/* An unmap changes only a page the
						 * generation or this batch holds. Most
						 * name pages nothing mapped, and a 4 KiB
						 * guest sends one per page, so plan none
						 * for them. */
						if (!page_indices.contains(page_address) &&
						    state.fill_index.lookup(page_address) == 0)
							continue;
						clear_page(page_for(page_address));
						continue;
					}
					auto &page = page_for(page_address);
					const std::optional<PageMapping> next{PageMapping{
						operation.mapping.backing.get(),
						operation.mapping.offset + position,
						operation.mapping.fd,
						operation.mapping.protection,
						operation.mapping.guest_code,
						operation.mapping.guest_writable}};
					if (same_mapping(page.final, next))
						continue;
					if (!operation_slot)
						operation_slot = state.backings.claim(
							operation.mapping.backing,
							operation.mapping.fd);
					changed = true;
					note_code_change(code, page.address,
						page.original, next);
					page.final = next;
					page.owner = &operation.mapping.backing;
					page.slot = *operation_slot;
				}
			}
			if (changed)
				changes.push_back({operation.address,
					operation.length, code});
		}
		if (changes.empty())
			return;

		reserve_planned_pages(state, pages);
		PendingMappingChanges pending{state, changes};
		commit_planned_pages(state, pages);
		pending.commit();
	}

	void map_locked(GenerationState &state, std::uint64_t address,
		std::uint64_t length, const Mapping &mapping)
	{
		SlotSettler settler{state};
		ScratchScope scratch;
		std::vector<PlannedMappingPage, ScratchAllocator<PlannedMappingPage>>
			pages{ScratchAllocator<PlannedMappingPage>{scratch.arena()}};
		pages.reserve(static_cast<std::size_t>(length / page_size));
		CodeSpan code;
		std::optional<unsigned> slot;
		for (std::uint64_t position = 0; position < length;
		     position += page_size) {
			const auto page_address = address + position;
			const auto previous = find_page(state, page_address);
			const std::optional<PageMapping> next{PageMapping{
				mapping.backing.get(), mapping.offset + position,
				mapping.fd, mapping.protection, mapping.guest_code,
				mapping.guest_writable}};
			if (same_mapping(previous, next))
				continue;
			if (!slot)
				slot = state.backings.claim(mapping.backing, mapping.fd);
			note_code_change(code, page_address, previous, next);
			pages.push_back({page_address, previous, next,
				&mapping.backing, *slot});
		}
		if (pages.empty())
			return;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		reserve_planned_pages(state, pages);
		PendingMappingChanges pending{state, changes};
		commit_planned_pages(state, pages);
		pending.commit();
	}

	[[nodiscard]] static bool batch_changes_published_mapping(
		const GenerationState &state,
		std::span<const MappingOperation> operations)
	{
		for (const auto &operation : operations) {
			const auto differs = [&](std::uint64_t address,
						 std::uintptr_t word) {
				if (!operation.map)
					return true;
				const auto mapping = decode_page(state, address, word);
				if (!mapping)
					return true;
				const auto position = address - operation.address;
				return mapping->backing != operation.mapping.backing.get() ||
					mapping->fd != operation.mapping.fd ||
					mapping->offset != operation.mapping.offset + position ||
					mapping->protection != operation.mapping.protection ||
					mapping->guest_code != operation.mapping.guest_code ||
					mapping->guest_writable !=
						operation.mapping.guest_writable;
			};
			if (state.fill_index.scan(operation.address,
				    operation.address + operation.length, differs))
				return true;
		}
		return false;
	}

	[[nodiscard]] bool unmap_locked(GenerationState &state,
		std::uint64_t address,
		std::uint64_t length)
	{
		/* exit_mmap() unmaps the whole user range of a generation that has
		 * few pages mapped, often none: the index walks only the subtrees that
		 * hold a page, whatever the width of the range. */
		const auto end = address + length;
		bool changed = false;
		CodeSpan code;
		(void)state.fill_index.scan(address, end,
			[&](std::uint64_t page, std::uintptr_t word) {
				changed = true;
				const auto fields = FillIndex::decode(word);
				if (fields.guest_code && fields.exec) {
					code.add(page);
					if (const auto mapping = decode_page(state, page, word))
						mapping->backing->retire_code_page(mapping->offset);
				}
				return false;
			});
		if (!changed)
			return false;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		PendingMappingChanges pending{state, changes};
		state.fill_index.erase_range(address, end,
			[&state](std::uint64_t page, std::uintptr_t word) noexcept {
				state.backings.remove(FillIndex::decode(word).slot, page);
				--state.mapped_pages;
			});
		state.backings.settle();
		pending.commit();
		return true;
	}

	static void invalidate_code_page(
		GenerationState &state, std::uint64_t address)
	{
		const auto page = address & ~(page_size - 1);
		std::shared_lock lock{state.mappings_mutex};
		const auto mapping = find_page(state, page);
		if (!mapping || !mapping->guest_code)
			return;
		MappedBacking::invalidate_code_page(
			mapping->backing->code_page_handle(mapping->offset));
	}

	static void publish_code_change(
		GenerationState &state, std::uint64_t address)
	{
		::uml::memory::publish_code_change(
			state, address & ~(page_size - 1), page_size);
	}

	/* Shared with every generation: the last holder gives the pages back. */
	std::shared_ptr<DomainHeap> heap_{std::make_shared<DomainHeap>()};
	mutable WordSharedMutex states_mutex_;
	WordSharedMutex backing_mutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<GenerationState>> states_;
	std::unordered_map<BackingKey, std::weak_ptr<MappedBacking>, BackingKeyHash>
		backings_;
	std::unordered_map<int, std::weak_ptr<MappedBacking>> descriptor_backings_;
	std::shared_ptr<AddressSpaceLifetime> lifetime_{
		std::make_shared<AddressSpaceLifetime>()};
	std::atomic_bool retired_{};
};

/* Holds space for one operation. It either owns a reference or borrows the
 * one in its thread's ThreadAddressSpaceCache; a borrowing access keeps that
 * cache entry pinned until it is destroyed. */
class AddressSpaceAccess final {
public:
	AddressSpaceAccess() noexcept = default;

	AddressSpaceAccess(AddressSpace *space,
		std::shared_ptr<AddressSpace> owned,
		ThreadAddressSpaceCache &cache)
		: owned_{std::move(owned)}, space_{space},
		  lender_{owned_ != nullptr ? nullptr : &cache},
		  slot_{&active_access_slot()}
	{
		if (lender_ != nullptr)
			++lender_->borrowed_accesses;
		slot_->space.store(space_, std::memory_order_seq_cst);
		if (!space_->retired_.load(std::memory_order_seq_cst))
			return;
		slot_->space.store(nullptr, std::memory_order_seq_cst);
		slot_ = nullptr;
		release();
		errno = ESHUTDOWN;
	}

	~AddressSpaceAccess()
	{
		if (slot_ != nullptr)
			slot_->space.store(nullptr, std::memory_order_seq_cst);
		release();
	}

	AddressSpaceAccess(AddressSpaceAccess &&other) noexcept
		: owned_{std::move(other.owned_)},
		  space_{std::exchange(other.space_, nullptr)},
		  lender_{std::exchange(other.lender_, nullptr)},
		  slot_{std::exchange(other.slot_, nullptr)}
	{
	}
	AddressSpaceAccess &operator=(AddressSpaceAccess &&) noexcept = delete;

	AddressSpaceAccess(const AddressSpaceAccess &) = delete;
	AddressSpaceAccess &operator=(const AddressSpaceAccess &) = delete;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return space_ != nullptr;
	}

	[[nodiscard]] AddressSpace &operator*() const noexcept
	{
		return *space_;
	}

	[[nodiscard]] AddressSpace *operator->() const noexcept
	{
		return space_;
	}

private:
	/* Runs after the slot is cleared, so a concurrent retire() can never be
	 * left waiting on a slot whose address space has already gone. */
	void release() noexcept
	{
		space_ = nullptr;
		owned_.reset();
		if (lender_ != nullptr)
			--std::exchange(lender_, nullptr)->borrowed_accesses;
	}

	std::shared_ptr<AddressSpace> owned_;
	AddressSpace *space_{};
	ThreadAddressSpaceCache *lender_{};
	ActiveAccessSlot *slot_{};
};

constexpr std::uint64_t legacy_domain = 1;

std::atomic<DomainProvider> &domain_provider()
{
	static std::atomic<DomainProvider> provider{};
	return provider;
}

std::atomic<DomainRetireObserver> &domain_retire_observer()
{
	static std::atomic<DomainRetireObserver> observer{};
	return observer;
}

#if UML_MEMORY_ALLOCATION_TESTING
std::atomic<testing::SoftwareAddressSpaceAcquiredHook> &
software_address_space_acquired_hook()
{
	static std::atomic<testing::SoftwareAddressSpaceAcquiredHook> hook{};
	return hook;
}
#endif

[[nodiscard]] bool resolve_current_domain_impl(std::uint64_t &domain) noexcept
{
	const auto provider = domain_provider().load(std::memory_order_acquire);
	if (provider == nullptr) {
		domain = legacy_domain;
		return true;
	}
	return provider(domain) && domain != 0;
}

/* Every software-TLB miss in both engines resolves the address space through
 * current(). Before the engine split that was a process-global instance reached
 * by a plain reference, so a miss cost one lock: the shared_lock on the
 * generation's mappings_mutex inside translate(). Domain support made it a
 * registry lookup, which adds a second acquisition of a process-global
 * shared_mutex plus a hash find to every miss on every vCPU thread - and
 * libc++'s shared_mutex takes a real internal std::mutex in lock_shared(), so
 * "shared" readers serialize on one cacheline rather than proceeding in
 * parallel. That cost scales with vCPU count, which is why it shows up in the
 * multi-core phase.
 *
 * The resolved owner is therefore cached per thread. activate() already keeps a
 * thread-local generation slot of exactly this shape. Validity is one relaxed
 * comparison against the global retirement sequence, which retire_domain()
 * advances before it touches the registry, so a cache hit proves no retirement
 * has begun since the entry was published. A hit lends the thread's own
 * reference (see ThreadAddressSpaceCache); a resolution made while a loan is
 * outstanding leaves the cache alone and returns an owning reference. */
struct ResolvedOwner final {
	AddressSpace *space{};
	/* Empty when space is lent from the thread's cache. */
	std::shared_ptr<AddressSpace> owned;
};

class AddressSpaceRegistry final {
public:
	[[nodiscard]] ResolvedOwner current(
		ThreadAddressSpaceCache &cache) noexcept
	{
		for (;;) {
			const auto retirement_sequence =
				domain_retirement_sequence_storage().load(
					std::memory_order_acquire);
			std::uint64_t domain;
			if (!resolve_current_domain_impl(domain)) {
				errno = ESRCH;
				return {};
			}
			auto &resolved = cache.resolved;
			/* A lent reference must outlive its access, so an outstanding
			 * loan freezes the entry and a miss resolves without it. */
			const bool cacheable = cache.borrowed_accesses == 0;
			if (resolved.owner != nullptr) {
				if (resolved.retirement_sequence == retirement_sequence &&
				    resolved.domain == domain)
					return ResolvedOwner{resolved.owner.get(), {}};
				/* Any retirement invalidates the entry, which also releases
				 * this thread's reference to a space that may be the retired
				 * one. */
				if (resolved.retirement_sequence != retirement_sequence &&
				    cacheable)
					resolved = {};
			}
			/* Only ever reached after the sequence has been re-read and found
			 * unchanged, so the published entry is one no retirement has
			 * touched. */
			const auto publish =
				[&](std::shared_ptr<AddressSpace> space) noexcept {
					if (!cacheable) {
						auto *raw = space.get();
						return ResolvedOwner{raw, std::move(space)};
					}
					resolved.domain = domain;
					resolved.retirement_sequence = retirement_sequence;
					resolved.owner = std::move(space);
					return ResolvedOwner{resolved.owner.get(), {}};
				};
			try {
				{
					std::shared_lock lock{mutex_};
					if (const auto found = spaces_.find(domain);
					    found != spaces_.end()) {
						if (domain_retirement_sequence_storage().load(
							    std::memory_order_acquire) !=
							    retirement_sequence)
							continue;
						return publish(found->second);
					}
				}
				std::lock_guard lock{mutex_};
				if (domain_retirement_sequence_storage().load(
					    std::memory_order_acquire) != retirement_sequence)
					continue;
				if (const auto found = spaces_.find(domain);
				    found != spaces_.end())
					return publish(found->second);
				auto space = std::make_shared<AddressSpace>();
				/* Retirement advances the sequence before acquiring mutex_. A resolver
				 * that captured a domain earlier therefore cannot install state after
				 * retirement. If retirement starts after this check, it waits for this
				 * publication and removes the entry. This needs no permanent tombstone
				 * or second provider lookup on the ordinary path. */
				if (domain_retirement_sequence_storage().load(
					    std::memory_order_acquire) != retirement_sequence)
					continue;
				spaces_.emplace(domain, space);
				return publish(std::move(space));
			} catch (const std::bad_alloc &) {
				errno = ENOMEM;
				return {};
			} catch (...) {
				errno = EIO;
				return {};
			}
		}
	}

	void retire(std::uint64_t domain) noexcept
	{
		std::shared_ptr<AddressSpace> retired;
		{
			std::lock_guard lock{mutex_};
			if (const auto found = spaces_.find(domain);
			    found != spaces_.end()) {
				retired = std::move(found->second);
				spaces_.erase(found);
			}
		}
		if (retired)
			retired->retire();
	}

#if UML_MEMORY_ALLOCATION_TESTING
	[[nodiscard]] std::size_t size_for_testing() const noexcept
	{
		std::shared_lock lock{mutex_};
		return spaces_.size();
	}
#endif

	void forget_descriptor(int fd)
	{
		std::shared_lock lock{mutex_};
		for (const auto &[domain, space] : spaces_) {
			(void)domain;
			space->forget_descriptor(fd);
		}
	}

private:
	mutable std::shared_mutex mutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<AddressSpace>> spaces_;
};

std::atomic<AddressSpaceRegistry *> published_address_space_registry{};
std::atomic_flag address_space_registry_initialization = ATOMIC_FLAG_INIT;

[[nodiscard]] AddressSpaceRegistry *existing_address_space_registry() noexcept
{
	return published_address_space_registry.load(std::memory_order_acquire);
}

AddressSpaceRegistry *address_space_registry() noexcept
{
	/* Context teardown can run during static destruction. Keep the registry at
	 * process lifetime so a late Context destructor never calls through
	 * an already destroyed mutex or container. Initialization is retryable: a
	 * transient allocation failure must not permanently cache a null registry. */
	return process_singleton(published_address_space_registry,
		address_space_registry_initialization,
		[]() noexcept -> AddressSpaceRegistry * {
			try {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::software_domain_registry);
				return new (std::nothrow) AddressSpaceRegistry;
			} catch (...) {
				return nullptr;
			}
		});
}

[[nodiscard]] AddressSpaceAccess address_space() noexcept
{
	try {
		auto &cache = thread_address_space_cache();
		prune_retired_thread_state(cache);
		errno = 0;
		auto *registry = address_space_registry();
		if (registry == nullptr) {
			errno = ENOMEM;
			return {};
		}
		auto owner = registry->current(cache);
		if (owner.space == nullptr)
			return {};
#if UML_MEMORY_ALLOCATION_TESTING
		if (const auto hook = software_address_space_acquired_hook().load(
			    std::memory_order_acquire); hook != nullptr) {
			/* The hook may resolve again, so a lent space stays pinned. */
			const bool lent = owner.owned == nullptr;
			cache.borrowed_accesses += lent;
			hook();
			cache.borrowed_accesses -= lent;
		}
#endif
		return AddressSpaceAccess{
			owner.space, std::move(owner.owned), cache};
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
	} catch (...) {
		errno = EIO;
	}
	return {};
}

} // namespace

void set_domain_provider(DomainProvider provider) noexcept
{
	domain_provider().store(provider, std::memory_order_release);
}

bool resolve_current_domain(std::uint64_t &domain) noexcept
{
	return resolve_current_domain_impl(domain);
}

std::uint64_t domain_retirement_sequence() noexcept
{
	return domain_retirement_sequence_storage().load(
		std::memory_order_acquire);
}

#if UML_MEMORY_ALLOCATION_TESTING
namespace testing {

std::size_t software_domain_count() noexcept
{
	auto *registry = existing_address_space_registry();
	return registry != nullptr ? registry->size_for_testing() : 0;
}

void set_software_address_space_acquired_hook(
	SoftwareAddressSpaceAcquiredHook hook) noexcept
{
	software_address_space_acquired_hook().store(
		hook, std::memory_order_release);
}

} // namespace testing
#endif

void set_domain_retire_observer(DomainRetireObserver observer) noexcept
{
	domain_retire_observer().store(observer, std::memory_order_release);
}

void retire_domain(std::uint64_t domain) noexcept
{
	if (domain == 0)
		return;
	domain_retirement_sequence_storage().fetch_add(
		1, std::memory_order_acq_rel);
	/* Retirement is a teardown operation. It must never allocate or initialize
	 * process-wide state merely to discover that the domain was untouched. */
	if (auto *registry = existing_address_space_registry(); registry != nullptr)
		registry->retire(domain);
	if (const auto observer = domain_retire_observer().load(
		    std::memory_order_acquire); observer != nullptr)
		observer(domain);
}

} // namespace uml::memory

namespace {

template<typename Function>
[[nodiscard]] int with_address_space(Function &&function) noexcept
{
	return uml::memory::bridge_call(
		[] { return uml::memory::address_space(); },
		std::forward<Function>(function),
		[] { uml::memory::reset_mapping_batch(); });
}

template<typename Function>
[[nodiscard]] std::uint64_t with_address_space_sequence(
	Function &&function) noexcept
{
	try {
		auto space = uml::memory::address_space();
		return space ? function(*space) : 0;
	} catch (...) {
		uml::memory::reset_mapping_batch();
		return 0;
	}
}

} // namespace

extern "C" int darwin_uml_close(int fd)
{
	try {
		if (auto *registry = uml::memory::existing_address_space_registry();
		    registry != nullptr)
			registry->forget_descriptor(fd);
	} catch (...) {
		/* Descriptor closure must still happen after bookkeeping failure. */
	}
	return ::close(fd);
}

extern "C" int darwin_uml_static_vm_map(void *address, int fd,
					 unsigned long long offset,
					 unsigned long length, int protection,
					 int guest_code, int guest_writable)
{
	return with_address_space([&](auto &space) {
		return space.map(space.active_generation(),
			reinterpret_cast<std::uintptr_t>(address), fd, offset, length,
			protection, guest_code != 0, guest_writable != 0);
	});
}

extern "C" int darwin_uml_static_vm_map_generation(
	unsigned long generation, void *address, int fd,
	unsigned long long offset, unsigned long length, int protection,
	int guest_code, int guest_writable)
{
	return with_address_space([&](auto &space) {
		return space.map(generation,
			reinterpret_cast<std::uintptr_t>(address), fd, offset,
			length, protection, guest_code != 0, guest_writable != 0);
	});
}

extern "C" int darwin_uml_static_vm_protect(void *address,
					     unsigned long length,
					     int protection)
{
	return with_address_space([&](auto &space) {
		return space.protect(
			reinterpret_cast<std::uintptr_t>(address), length, protection);
	});
}

extern "C" int darwin_uml_static_vm_unmap(void *address,
					   unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.unmap(space.active_generation(),
			reinterpret_cast<std::uintptr_t>(address), length);
	});
}

extern "C" int darwin_uml_static_vm_unmap_generation(
	unsigned long generation, void *address, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.unmap(generation,
			reinterpret_cast<std::uintptr_t>(address), length);
	});
}

extern "C" int darwin_uml_static_vm_flush_generation(
	unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.flush_generation(generation); });
}

extern "C" int darwin_uml_static_vm_activate(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.activate(generation); });
}

extern "C" int darwin_uml_static_vm_destroy(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.destroy(generation); });
}

extern "C" int darwin_uml_static_vm_generation_live(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.generation_live(generation); });
}

extern "C" unsigned long long darwin_uml_static_vm_generation_retirements(void)
{
	return uml::memory::generation_retirement_sequence().load(
		std::memory_order_acquire);
}

extern "C" int darwin_uml_static_vm_discard_if_stale(unsigned long address,
	int fd, unsigned long long offset)
{
	return with_address_space([&](auto &space) {
		return space.discard_if_stale(address, fd, offset);
	});
}

extern "C" int darwin_uml_static_vm_copy_from_buffer(
	unsigned long address, const void *buffer, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.copy_from_buffer(address, buffer, length, false);
	});
}

extern "C" int darwin_uml_static_vm_copy_kernel_write(
	unsigned long address, const void *buffer, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.copy_from_buffer(address, buffer, length, true);
	});
}

extern "C" void darwin_uml_static_vm_require_coherent_kernel_code_writes(void)
{
	auto &required = uml::memory::coherent_kernel_code_writes();
	if (required.load(std::memory_order_acquire))
		return;
	required.store(true, std::memory_order_seq_cst);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

extern "C" int darwin_uml_static_vm_retry_fault(
	unsigned long address, int execute, int write)
{
	return with_address_space([&](auto &space) {
		void *translated = nullptr;
		const auto access = execute != 0 ? DARWIN_UML_STATIC_EXECUTE :
			write != 0 ? DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
		const int result = space.translate(
			space.active_generation(), address, access, &translated);
		if (result == DARWIN_UML_STATIC_TRANSLATED &&
		    translated != nullptr)
			return 1;
		return result == DARWIN_UML_STATIC_HOST_ERROR ? -1 : 0;
	});
}

extern "C" int darwin_uml_static_vm_batch_begin(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.begin_batch(generation); });
}

extern "C" int darwin_uml_static_vm_batch_end(void)
{
	return with_address_space([](auto &space) { return space.end_batch(); });
}

extern "C" int darwin_uml_static_vm_walker_lock(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.lock_walker(generation); });
}

extern "C" int darwin_uml_static_vm_walker_unlock(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.unlock_walker(generation); });
}

extern "C" int darwin_uml_static_vm_translate(
	unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address)
{
	return with_address_space([&](auto &space) {
		return space.translate(generation, address, access, host_address);
	});
}

extern "C" int darwin_uml_static_vm_execution_context_translate(
	void *context, unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address)
{
	const int result = uml::memory::AddressSpace::execution_context_translate(
		static_cast<uml::memory::ExecutionContext *>(context), generation,
		address, access, host_address);
	if (result != uml::memory::AddressSpace::execution_context_fallback)
		return result;
	return darwin_uml_static_vm_translate(generation, address, access,
		host_address);
}

extern "C" int darwin_uml_static_vm_code_page(
	unsigned long generation, unsigned long address, void **handle,
	struct darwin_uml_static_code_page_stamp *stamp)
{
	return with_address_space([&](auto &space) {
		return space.code_page(generation, address, handle, stamp);
	});
}

extern "C" int darwin_uml_static_vm_code_page_stamp(
	const void *handle, struct darwin_uml_static_code_page_stamp *stamp)
{
	if (stamp == nullptr)
		return -1;
	*stamp = {};
	return uml::memory::MappedBacking::load_code_page_stamp(
		handle, stamp->identity, stamp->version) ? 0 : -1;
}

extern "C" void darwin_uml_static_vm_invalidate_code_page(void *handle)
{
	uml::memory::MappedBacking::invalidate_code_page(handle);
}

extern "C" uint64_t darwin_uml_static_vm_changes(unsigned long generation)
{
	return with_address_space_sequence(
		[&](auto &space) { return space.changes(generation); });
}

extern "C" int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence)
{
	return with_address_space([&](auto &space) {
		return space.mapping_changes(
			generation, sequence, changes, capacity, current_sequence);
	});
}

extern "C" int darwin_uml_static_vm_execution_context_code_change_ranges(
	void *context, uint64_t sequence,
	struct darwin_uml_static_code_change *changes, size_t capacity,
	uint64_t *current_sequence)
{
	return uml::memory::execution_context_code_changes(context, sequence,
		changes, capacity, current_sequence);
}

extern "C" void *darwin_uml_static_vm_execution_context_create(
	unsigned long generation)
{
	try {
		auto space = uml::memory::address_space();
		return space ? space->create_execution_context(generation) : nullptr;
	} catch (...) {
		return nullptr;
	}
}

extern "C" void darwin_uml_static_vm_execution_context_destroy(void *context)
{
	try {
		uml::memory::AddressSpace::destroy_execution_context(
			static_cast<uml::memory::ExecutionContext *>(context));
	} catch (...) {
		/* The C destruction boundary cannot report an error. */
	}
}

extern "C" int darwin_uml_static_vm_execution_context_begin(
	void *context, unsigned long guest_pc)
{
	try {
		errno = 0;
		const int result = uml::memory::AddressSpace::execution_context_begin(
			static_cast<uml::memory::ExecutionContext *>(context), guest_pc);
		if (result < 0 && errno == 0)
			errno = EFAULT;
		return result;
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return -1;
	} catch (...) {
		errno = EIO;
		return -1;
	}
}

extern "C" int darwin_uml_static_vm_execution_context_end(void *context)
{
	try {
		errno = 0;
		const int result = uml::memory::AddressSpace::execution_context_end(
			static_cast<uml::memory::ExecutionContext *>(context));
		if (result < 0 && errno == 0)
			errno = EFAULT;
		return result;
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return -1;
	} catch (...) {
		errno = EIO;
		return -1;
	}
}

extern "C" struct darwin_uml_static_sequences
darwin_uml_static_vm_execution_context_sequences(void *context)
{
	try {
		return uml::memory::AddressSpace::execution_context_sequences(
			static_cast<uml::memory::ExecutionContext *>(context));
	} catch (...) {
		return {};
	}
}

extern "C" const uint64_t *
darwin_uml_static_vm_execution_context_code_sequence(void *context)
{
	return uml::memory::AddressSpace::execution_context_code_sequence(
		static_cast<uml::memory::ExecutionContext *>(context));
}

extern "C" int darwin_uml_static_vm_invalidate_code_generation(
	unsigned long generation, unsigned long address, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.invalidate_code(generation, address, length);
	});
}

extern "C" void darwin_uml_static_vm_invalidate_code(unsigned long address)
{
	(void)with_address_space([&](auto &space) {
		space.invalidate_code(address);
		return 0;
	});
}
