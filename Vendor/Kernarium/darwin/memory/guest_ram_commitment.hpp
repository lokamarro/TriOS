// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::memory {

/* Which host pages of guest RAM the guest holds.
 *
 * mem= is a ceiling. The guest commits a range before it uses it and releases
 * a range it no longer uses; a released page is dropped from the process
 * footprint and, where guest RAM is wired, unwired. Darwin keeps a released
 * page's contents only until it wants the memory and stops counting it, so a
 * page written again without a commit would be used but not counted - the
 * process could pass its memory limit unseen - and would stay unwired.
 *
 * Offsets index the physical-memory backing, the guest's physical addresses.
 * One bit per host page records the state and only a page whose state changes
 * reaches the host, so repeated, overlapping and concurrent calls are safe.
 * Offsets below `managed_begin` name memory this object does not own, the
 * signed kernel image, and are accepted and ignored. */
class GuestRamCommitment final {
public:
	/* Pages from `uncommitted_begin` start released; pages before it start
	 * committed (and, with `wire`, are expected to be wired already). */
	GuestRamCommitment(std::byte *physical_root, std::size_t size,
		std::size_t managed_begin, std::size_t uncommitted_begin,
		bool wire, std::size_t page_size);
	GuestRamCommitment(const GuestRamCommitment &) = delete;
	GuestRamCommitment &operator=(const GuestRamCommitment &) = delete;

	/* 0 on success, -1 with errno set otherwise. EINVAL for a range that is
	 * not page aligned or leaves the backing. */
	[[nodiscard]] int release(std::uint64_t offset,
		std::uint64_t length) noexcept;
	[[nodiscard]] int commit(std::uint64_t offset,
		std::uint64_t length) noexcept;

	/* Offsets below this belong to the signed kernel image; they are never
	 * released, so whoever maps them keeps them wired. */
	[[nodiscard]] std::size_t managed_begin() const noexcept;
	[[nodiscard]] std::size_t managed_bytes() const noexcept;
	[[nodiscard]] std::size_t released_bytes() const noexcept;
	[[nodiscard]] std::uint64_t wire_failures() const noexcept;

	/* The geometry and state a snapshot of the committed pages needs. A page
	 * is "held" when it is managed and the guest has committed it. */
	[[nodiscard]] std::byte *root() const noexcept { return root_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] std::size_t page_size() const noexcept
	{
		return page_size_;
	}
	[[nodiscard]] std::size_t page_count() const noexcept
	{
		return page_count_;
	}
	[[nodiscard]] bool wires() const noexcept { return wire_; }
	/* Bit i of word w is set when page 64 * w + i is held. */
	[[nodiscard]] std::size_t word_count() const noexcept;
	[[nodiscard]] std::uint64_t held_word(std::size_t word) const noexcept;

	/* Holds back every commit and release until `allow_changes`, so that a
	 * snapshot of the held pages cannot move under whoever takes it. A call
	 * that arrives meanwhile waits before it changes any state. Evict and
	 * readmit below are the only changes allowed in the interval.
	 *
	 * A call already past that wait is in flight, and a thread stopped inside
	 * one would finish it after the snapshot. The caller therefore stops every
	 * thread that can call, holds changes, and then requires
	 * `changes_in_flight` to be zero; otherwise it allows changes, lets the
	 * threads go and tries again. */
	void hold_changes() noexcept;
	void allow_changes() noexcept;
	[[nodiscard]] std::size_t changes_in_flight() const noexcept;
	[[nodiscard]] bool changes_held() const noexcept
	{
		return changes_held_.load(std::memory_order_acquire);
	}

	/* Takes held pages off the footprint without changing what the guest
	 * holds: unwires them (where guest RAM is wired) and marks them
	 * reusable, after which Darwin may drop their contents at any time. The
	 * caller has written them elsewhere. Readmit counts them again and wires
	 * them; their contents are then undefined until the caller refills them.
	 * 0 on success, -1 with errno set. */
	[[nodiscard]] int evict(std::size_t first_page,
		std::size_t pages) noexcept;
	[[nodiscard]] int readmit(std::size_t first_page,
		std::size_t pages) noexcept;

private:
	enum class Change : std::uint8_t { release, commit };

	[[nodiscard]] int change(Change kind, std::uint64_t offset,
		std::uint64_t length) noexcept;
	[[nodiscard]] int apply(Change kind, std::size_t first_page,
		std::size_t pages) noexcept;

	std::byte *root_;
	std::size_t size_;
	std::size_t managed_begin_;
	std::size_t page_size_;
	std::size_t page_count_;
	bool wire_;
	/* Bit set: the page is released (or was never committed). */
	std::unique_ptr<std::atomic<std::uint64_t>[]> released_;
	std::atomic<std::size_t> released_pages_{0};
	std::atomic<std::uint64_t> wire_failures_{0};
	/* Set while a snapshot of the held pages is out; see hold_changes. */
	std::atomic<bool> changes_held_{false};
	/* Commits and releases that have passed the wait on changes_held_ and not
	 * yet returned. */
	std::atomic<std::size_t> in_flight_{0};
};

/* The process's guest RAM, published once the kernel's physical window exists.
 * Null before that, and again once the guest has been handed back. */
[[nodiscard]] GuestRamCommitment *guest_ram_commitment() noexcept;
void publish_guest_ram_commitment(
	std::unique_ptr<GuestRamCommitment> commitment) noexcept;

/* Takes the published commitment back, so that the next guest publishes its
 * own. The window it describes is gone or about to be: the caller (the guest
 * RAM hand-back, shared_backing.hpp) has no thread left that can commit or
 * release, and holds no snapshot of the held pages. */
void retire_guest_ram_commitment() noexcept;

/* Bridge entry points: 0 on success, -1 with errno. Before guest RAM exists
 * there is nothing to change and they fail with ENXIO. */
[[nodiscard]] int release_guest_ram(std::uint64_t offset,
	std::uint64_t length) noexcept;
[[nodiscard]] int commit_guest_ram(std::uint64_t offset,
	std::uint64_t length) noexcept;

} // namespace uml::memory
