// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::gadget_cache {

/* The machinery by which gadget-engine executors share an address space's
 * translations: what each executor says about itself, the executors attached
 * to an address space, the limbo a body waits in before it is freed, the
 * counts that decide a shared space is renewed, and the registry that hands an
 * address space out. An engine keeps its own body, record and space types and
 * builds its space from these parts. */

/* What each executor says about itself for the bodies other executors free.
 *
 * Between begin_execution() and end_execution() an executor may hold a stream
 * pointer it loaded from a link, so a body unpublished in that time may only
 * be freed once the executor has left. The sequence is odd from entry until
 * exit and even outside, and moves at both, so a reclaimer that saw an odd
 * value waits for any other. An executor that never brackets its runs stays
 * odd, which only keeps bodies longer. A guest entry runs at most a dispatch
 * budget, so a running executor leaves within a fraction of a millisecond. */
struct alignas(128) ExecutorAnnouncement final {
	std::atomic<std::uint64_t> sequence{};
	bool used{};
	/* Set by whichever executor closes an address space this one is
	 * attached to (Space::request_renewal): the next time this executor
	 * enters translated code it first lets go of every closed address
	 * space it holds. Written through a pointer to const, since an
	 * executor sees the announcements of the others that way. */
	mutable std::atomic<bool> renew{};
};

/* Publish a pair of words that translated code reads with one `ldp`: a
 * BlockLink's stream and instruction count, or an indirect cell's guest PC
 * and link. With FEAT_LSE2 an aligned sixteen-byte `stp` is a single access,
 * so a vCPU following the pair while another retargets it sees one value or
 * the other, never a stream with another block's count. */
inline void store_pair(void *at, std::uint64_t first,
	std::uint64_t second) noexcept
{
	assert((reinterpret_cast<std::uintptr_t>(at) & 15U) == 0U);
	asm volatile("stp %x1, %x2, [%x0]"
		     :
		     : "r"(at), "r"(first), "r"(second)
		     : "memory");
}

/* The pair store_pair publishes, read with one `ldp`. */
inline void load_pair(const void *at, std::uint64_t &first,
	std::uint64_t &second) noexcept
{
	assert((reinterpret_cast<std::uintptr_t>(at) & 15U) == 0U);
	asm volatile("ldp %x0, %x1, [%x2]"
		     : "=r"(first), "=r"(second)
		     : "r"(at)
		     : "memory");
}

[[nodiscard]] inline std::uint64_t word_of(const void *pointer) noexcept
{
	return reinterpret_cast<std::uintptr_t>(pointer);
}

inline constexpr std::size_t announcement_capacity = 256;
inline ExecutorAnnouncement announcements[announcement_capacity];
inline std::mutex announcements_mutex;

[[nodiscard]] constexpr bool inside(std::uint64_t sequence) noexcept
{
	return (sequence & 1U) != 0U;
}

[[nodiscard]] constexpr std::uint64_t next_odd(std::uint64_t sequence) noexcept
{
	return sequence + (inside(sequence) ? 2U : 1U);
}

[[nodiscard]] inline ExecutorAnnouncement *acquire_announcement() noexcept
{
	const std::lock_guard guard{announcements_mutex};
	for (auto &announcement : announcements) {
		if (announcement.used)
			continue;
		announcement.used = true;
		announcement.renew.store(false, std::memory_order_relaxed);
		announcement.sequence.store(next_odd(announcement.sequence.load(
			std::memory_order_relaxed)), std::memory_order_relaxed);
		return &announcement;
	}
	return nullptr;
}

/* The executor may now load stream pointers from links: every body
 * unpublished before this fence is out of its reach. */
inline void announce_entry(ExecutorAnnouncement &announcement) noexcept
{
	announcement.sequence.store(next_odd(announcement.sequence.load(
		std::memory_order_relaxed)), std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

/* The executor holds no stream pointer any more. */
inline void announce_exit(ExecutorAnnouncement &announcement) noexcept
{
	const auto sequence =
		announcement.sequence.load(std::memory_order_relaxed);
	if (inside(sequence))
		announcement.sequence.store(sequence + 1U,
			std::memory_order_release);
}

inline void release_announcement(ExecutorAnnouncement *announcement) noexcept
{
	if (announcement == nullptr)
		return;
	const auto sequence =
		announcement->sequence.load(std::memory_order_relaxed);
	if (inside(sequence))
		announcement->sequence.store(sequence + 1U,
			std::memory_order_release);
	const std::lock_guard guard{announcements_mutex};
	announcement->used = false;
}

/* The executors of one address space whose leaving a reclaimer waits for, as
 * they were when it unpublished the bodies it holds back. */
struct Snapshot final {
	struct Seen final {
		const ExecutorAnnouncement *announcement;
		std::uint64_t sequence;
	};
	std::vector<Seen> seen;

	/* Every executor seen inside has left since, apart from `self`, which
	 * holds nothing it could reach through an unpublished body: releases
	 * happen in C++ before it runs translated code again. */
	[[nodiscard]] bool elapsed(const ExecutorAnnouncement *self) const noexcept
	{
		for (const auto &entry : seen) {
			if (entry.announcement == self || !inside(entry.sequence))
				continue;
			if (entry.announcement->sequence.load(
				    std::memory_order_acquire) == entry.sequence)
				return false;
		}
		return true;
	}
};

/* The executors an address space is attached to: the ones that may run its
 * bodies, and so the ones a reclaimer waits for and a renewal asks to let go.
 * Under SpaceRegistry the registry's lock also covers attaching. */
class AttachedExecutors final {
public:
	void attach(const ExecutorAnnouncement *announcement)
	{
		const std::lock_guard guard{mutex_};
		attached_.push_back(announcement);
	}

	/* Whether this was the last executor. */
	[[nodiscard]] bool detach(const ExecutorAnnouncement *announcement)
	{
		const std::lock_guard guard{mutex_};
		const auto found = std::find(attached_.begin(), attached_.end(),
			announcement);
		if (found != attached_.end())
			attached_.erase(found);
		return attached_.empty();
	}

	/* Ask every executor attached to let go of the address space before it
	 * next enters translated code. */
	void request_renewal()
	{
		const std::lock_guard guard{mutex_};
		for (const auto *announcement : attached_)
			if (announcement != nullptr)
				announcement->renew.store(true,
					std::memory_order_release);
	}

	/* Note where every executor attached is, or, without the memory for
	 * that, leave `snapshot` as it was and say so. */
	[[nodiscard]] bool take_snapshot(Snapshot &snapshot) const noexcept
	{
		const std::lock_guard guard{mutex_};
		try {
			snapshot.seen.reserve(attached_.size());
		} catch (const std::bad_alloc &) {
			return false;
		}
		snapshot.seen.clear();
		for (const auto *announcement : attached_)
			if (announcement != nullptr)
				snapshot.seen.push_back({announcement,
					announcement->sequence.load(
						std::memory_order_acquire)});
		return true;
	}

	[[nodiscard]] std::size_t size() const
	{
		const std::lock_guard guard{mutex_};
		return attached_.size();
	}

private:
	mutable std::mutex mutex_;
	std::vector<const ExecutorAnnouncement *> attached_;
};

/* Where bodies wait after they are unpublished, until no executor can still be
 * running them.
 *
 * A body a slot let go of is unpublished at once, but executors that loaded the
 * record before may still be running it, so it goes to `incoming`; the first
 * reclaim afterwards moves it behind a snapshot of the attached executors, and
 * a reclaim that finds every executor seen inside has left frees it. A caller
 * that cannot take the lock, finds executors still inside, or has no memory
 * for the snapshot never waits: the bodies stay for the next call. Neither
 * queueing nor reclaiming throws, so that letting go of a body is never left
 * half done. */
template <typename Body>
class Limbo final {
public:
	/* Without the memory to queue it, the body stays where it is,
	 * unreachable, until its arena goes with the address space. */
	void push(Body *body) noexcept
	{
		const std::lock_guard guard{mutex_};
		try {
			incoming_.push_back(body);
		} catch (const std::bad_alloc &) {
			return;
		}
		pending_.store(true, std::memory_order_relaxed);
	}

	/* Free what was unpublished long enough ago that no other executor can
	 * still be running it, through `free_bodies(std::vector<Body *> &)`.
	 * Never waits. */
	template <typename Free>
	void reclaim(const ExecutorAnnouncement *self,
		const AttachedExecutors &attached, Free &&free_bodies) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<Free &,
			std::vector<Body *> &>);
		if (!pending_.load(std::memory_order_relaxed))
			return;
		std::unique_lock limbo{mutex_, std::try_to_lock};
		if (!limbo.owns_lock())
			return;
		if (!waiting_.empty()) {
			if (!snapshot_.elapsed(self))
				return;
			free_bodies(waiting_);
		}
		if (!incoming_.empty()) {
			/* Every body here was unpublished before this fence, so
			 * an executor seen outside now can only enter after it
			 * and never reach one. The bodies wait behind the
			 * snapshot only once it is whole. */
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (attached.take_snapshot(snapshot_)) {
				waiting_.swap(incoming_);
				if (snapshot_.elapsed(self))
					free_bodies(waiting_);
			}
		}
		pending_.store(!waiting_.empty() || !incoming_.empty(),
			std::memory_order_relaxed);
	}

	[[nodiscard]] std::size_t size() const
	{
		const std::lock_guard limbo{mutex_};
		return waiting_.size() + incoming_.size();
	}

	/* Visit every body still waiting, for the space that is going away: no
	 * executor can run one, so each may be finished with at once. */
	template <typename Visit>
	void for_each(Visit &&visit)
	{
		const std::lock_guard limbo{mutex_};
		for (auto *body : waiting_)
			visit(body);
		for (auto *body : incoming_)
			visit(body);
		waiting_.clear();
		incoming_.clear();
		pending_.store(false, std::memory_order_relaxed);
	}

private:
	mutable std::mutex mutex_;
	/* Unpublished, not yet covered by a snapshot. */
	std::vector<Body *> incoming_;
	/* Unpublished before `snapshot_` was taken. */
	std::vector<Body *> waiting_;
	Snapshot snapshot_;
	std::atomic<bool> pending_{};
};

/* What decides a shared address space is renewed, and that it was.
 *
 * It counts the bytes of its arena, and of the bodies some slot holds (a body
 * counts from its first holder to its last). Once the arena holds at least as
 * much freed space as bodies, and at least the floor, the space is marked for
 * renewal. Both counts move apart from the decision, so this only ever errs
 * by a store or a release. */
class RenewalLedger final {
public:
	/* The arena's bytes changed hands: record them, and say whether they
	 * differ from what was last recorded. */
	bool note_arena(std::size_t bytes) noexcept
	{
		return arena_bytes_.exchange(bytes, std::memory_order_relaxed) !=
			bytes;
	}

	/* A body some slot holds now, where none did. */
	void note_held(std::size_t bytes) noexcept
	{
		held_bytes_.fetch_add(bytes, std::memory_order_relaxed);
	}

	/* The last slot let go of a body. */
	void note_released(std::size_t bytes, std::size_t floor) noexcept
	{
		held_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
		note_idle(floor);
	}

	/* Mark the space for renewal once the arena holds more bytes no body
	 * uses than bodies do, and at least `floor`. */
	void note_idle(std::size_t floor) noexcept
	{
		const auto arena = arena_bytes_.load(std::memory_order_relaxed);
		const auto held = held_bytes_.load(std::memory_order_relaxed);
		if (arena > held && arena - held >= held && arena - held >= floor)
			renewal_due_.store(true, std::memory_order_relaxed);
	}

	void mark_due() noexcept
	{
		renewal_due_.store(true, std::memory_order_relaxed);
	}

	/* Whether the space holds enough freed arena to be replaced and has not
	 * been. */
	[[nodiscard]] bool renewal_due() const noexcept
	{
		return renewal_due_.load(std::memory_order_relaxed) &&
			!closed_.load(std::memory_order_relaxed);
	}

	[[nodiscard]] bool closed() const noexcept
	{
		return closed_.load(std::memory_order_acquire);
	}

	/* Close the address space; true for the caller that did, which then
	 * withdraws it from the registry and asks its executors to let go. */
	[[nodiscard]] bool close() noexcept
	{
		return !closed_.exchange(true, std::memory_order_acq_rel);
	}

	[[nodiscard]] std::size_t arena_bytes() const noexcept
	{
		return arena_bytes_.load(std::memory_order_relaxed);
	}

	[[nodiscard]] std::size_t held_bytes() const noexcept
	{
		return held_bytes_.load(std::memory_order_relaxed);
	}

private:
	std::atomic<std::size_t> arena_bytes_{};
	std::atomic<std::size_t> held_bytes_{};
	std::atomic<bool> renewal_due_{};
	std::atomic<bool> closed_{};
};

/* The address spaces whose translations executors share, by key.
 *
 * `Space` is the engine's space and `Key` what makes two executors'
 * translations interchangeable: the guest address space (`generation`), the
 * engine's execution `mode`, the `options` they were built with, and an
 * `owner` that, when set, keeps the space private to that cache and out of the
 * registry. A Space is made from (Key, code-change sequence) and offers
 * key(), attach(), detach(), close(), closed(), shedding(), request_renewal(),
 * and the static note_closed() the registry calls for each space it closes. */
template <typename Space, typename Key>
class SpaceRegistry final {
public:
	[[nodiscard]] static SpaceRegistry &instance() noexcept
	{
		static SpaceRegistry registry;
		return registry;
	}

	/* The space for `key`, made when absent, with `announcement` among
	 * the executors a reclaimer waits for. A key with an owner is never
	 * shared and never registered. */
	[[nodiscard]] Space *attach(Key key, std::uint64_t changes,
		const ExecutorAnnouncement *announcement)
	{
		if (key.owner != nullptr) {
			auto *space = new Space(std::move(key), changes);
			space->attach(announcement);
			return space;
		}
		const std::lock_guard guard{mutex_};
		auto &candidates = spaces_[{key.generation, key.mode}];
		for (std::size_t index = 0; index < candidates.size();) {
			auto *space = candidates[index];
			/* One the budget asked to shed is closed here instead, as
			 * close() would, if no executor holding it has yet. */
			if (space->shedding()) {
				if (space->close()) {
					Space::note_closed();
					space->request_renewal();
				}
				candidates.erase(candidates.begin() +
					static_cast<std::ptrdiff_t>(index));
				continue;
			}
			if (space->key().options == key.options) {
				space->attach(announcement);
				return space;
			}
			++index;
		}
		auto space = std::make_unique<Space>(key, changes);
		space->attach(announcement);
		candidates.push_back(space.get());
		return space.release();
	}

	void detach(Space *space, const ExecutorAnnouncement *announcement)
	{
		if (space->key().owner != nullptr) {
			(void)space->detach(announcement);
			delete space;
			return;
		}
		std::unique_ptr<Space> destroyed;
		{
			const std::lock_guard guard{mutex_};
			if (!space->detach(announcement))
				return;
			withdraw(space);
			destroyed.reset(space);
		}
	}

	/* Close a shared space and ask its executors to let go of it. It stays
	 * alive for as long as some of them have not; no executor attaches to it
	 * from here on, since attaching happens under the lock that withdraws
	 * it, so every executor that did is among those asked. */
	void close(Space *space)
	{
		if (!space->close())
			return;
		Space::note_closed();
		const std::lock_guard guard{mutex_};
		withdraw(space);
		space->request_renewal();
	}

	/* The open spaces of a guest address space, all modes. */
	[[nodiscard]] std::vector<Space *> open_spaces(unsigned long generation)
	{
		const std::lock_guard guard{mutex_};
		std::vector<Space *> found;
		for (const auto &[key, candidates] : spaces_)
			if (key.generation == generation)
				for (auto *space : candidates)
					if (!space->closed())
						found.push_back(space);
		return found;
	}

private:
	struct SpaceId final {
		unsigned long generation;
		std::uint8_t mode;
		bool operator==(const SpaceId &) const noexcept = default;
	};
	struct SpaceIdHash final {
		[[nodiscard]] std::size_t operator()(const SpaceId &key) const noexcept
		{
			return std::hash<unsigned long>{}(key.generation) ^
				(std::size_t{key.mode} << 56);
		}
	};

	/* Stop handing a space out. It may already be gone, when it was closed.
	 * The caller holds mutex_. */
	void withdraw(const Space *space)
	{
		const auto found = spaces_.find(
			{space->key().generation, space->key().mode});
		if (found == spaces_.end())
			return;
		auto &candidates = found->second;
		const auto at =
			std::find(candidates.begin(), candidates.end(), space);
		if (at != candidates.end())
			candidates.erase(at);
		if (candidates.empty())
			spaces_.erase(found);
	}

	std::mutex mutex_;
	std::unordered_map<SpaceId, std::vector<Space *>, SpaceIdHash> spaces_;
};

} // namespace uml::cpu::gadget_cache
