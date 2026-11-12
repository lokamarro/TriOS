// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "fill_index.hpp"
#include "mapped_backing.hpp"
#include "page_index.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace uml::memory {

/* The backings and descriptors a generation's fill-index words name.
 *
 * A word has six bits for them, so a generation numbers its distinct
 * (backing, descriptor) pairs 0 to 62 and each slot counts the pages that
 * lie in it. A guest process maps one or two (the guest RAM file, mostly), so
 * the table is tiny and a page pays six bits instead of a reference to
 * its backing. A slot is released when its count reaches 0.
 *
 * A process can map more: many GPU blobs, one per descriptor. Slot 63 sends a
 * reader to an overflow table that records the pair per page, which costs what
 * a page cost before slots and keeps the index exact whatever the guest maps.
 *
 * Writers hold the generation's mapping lock exclusive and readers hold it
 * shared; nothing here is read without it. A change is made in steps so that
 * the step that can fail comes first:
 *
 *   claim()            finds or takes the slot of a pair; no allocation;
 *   reserve_overflow() makes room for a page in the overflow table, the only
 *                      step that allocates;
 *   add(), remove()    count the page in or out; no allocation;
 *   settle()           releases the slots a change left empty and the room
 *                      reserved but not used, and so also undoes a change
 *                      that stopped after its claims.
 */
class BackingSlots final {
public:
	static constexpr unsigned slot_count = FillIndex::overflow_slot;
	static constexpr unsigned overflow = FillIndex::overflow_slot;

	struct Entry final {
		MappedBacking *backing;
		int fd;
	};

	/* What the word of `page`, which names `slot`, maps into. A null backing
	 * means the table holds nothing for the page. */
	[[nodiscard]] Entry entry(unsigned slot, std::uint64_t page) const noexcept
	{
		if (slot < slot_count)
			return {slots_[slot].backing.get(), slots_[slot].fd};
		const auto found = overflow_.find(page);
		return found == overflow_.end() ? Entry{nullptr, -1} :
			Entry{found->second.backing.get(), found->second.fd};
	}

	/* The slot of the pair, taking a free one with no pages yet if it has
	 * none; `overflow` when all are taken. */
	[[nodiscard]] unsigned claim(
		const std::shared_ptr<MappedBacking> &backing, int fd) noexcept
	{
		unsigned free_slot = overflow;
		for (unsigned slot = 0; slot < slot_count; ++slot) {
			const auto &held = slots_[slot];
			if (held.backing == backing && held.fd == fd)
				return slot;
			if (!held.backing && free_slot == overflow)
				free_slot = slot;
		}
		if (free_slot != overflow) {
			slots_[free_slot] = {backing, fd, 0};
			touched_ |= std::uint64_t{1} << free_slot;
		}
		return free_slot;
	}

	/* Makes room for `page` in the overflow table. Throws std::bad_alloc. */
	void reserve_overflow(std::uint64_t page)
	{
		if (overflow_.try_emplace(page).second)
			++unfilled_;
	}

	/* Counts `page` into `slot`, or records its pair in the overflow table,
	 * where reserve_overflow() made room. */
	void add(unsigned slot, std::uint64_t page,
		const std::shared_ptr<MappedBacking> &backing, int fd) noexcept
	{
		if (slot < slot_count) {
			++slots_[slot].pages;
			return;
		}
		const auto found = overflow_.find(page);
		if (found == overflow_.end())
			return;
		if (!found->second.backing)
			--unfilled_;
		found->second = {backing, fd};
	}

	/* Counts `page` out of `slot`, or drops it from the overflow table. */
	void remove(unsigned slot, std::uint64_t page) noexcept
	{
		if (slot < slot_count) {
			--slots_[slot].pages;
			touched_ |= std::uint64_t{1} << slot;
			return;
		}
		overflow_.erase(page);
	}

	/* Releases the slots a change claimed or emptied that hold no page, and
	 * drops the overflow room it reserved but did not fill. Writers only. */
	void settle() noexcept
	{
		for (auto bits = touched_; bits != 0; bits &= bits - 1U) {
			auto &held = slots_[std::countr_zero(bits)];
			if (held.pages == 0)
				held = {};
		}
		touched_ = 0;
		if (unfilled_ != 0) {
			std::erase_if(overflow_, [](const auto &page) {
				return !page.second.backing;
			});
			unfilled_ = 0;
		}
	}

	void clear() noexcept
	{
		for (auto &held : slots_)
			held = {};
		overflow_.clear();
		touched_ = 0;
		unfilled_ = 0;
	}

	/* For tests: the slots that name a backing, the pages in one, and the
	 * pages in the overflow table. */
	[[nodiscard]] std::size_t slots_in_use() const noexcept
	{
		std::size_t count = 0;
		for (const auto &held : slots_)
			count += held.backing ? 1U : 0U;
		return count;
	}

	[[nodiscard]] std::uint64_t pages_in(unsigned slot) const noexcept
	{
		return slot < slot_count ? slots_[slot].pages : 0;
	}

	[[nodiscard]] std::size_t overflow_pages() const noexcept
	{
		return overflow_.size();
	}

private:
	struct Slot final {
		std::shared_ptr<MappedBacking> backing;
		int fd{-1};
		std::uint64_t pages{};
	};
	struct OverflowPage final {
		std::shared_ptr<MappedBacking> backing;
		int fd{-1};
	};

	std::array<Slot, slot_count> slots_{};
	std::unordered_map<std::uint64_t, OverflowPage, PageAddressHash>
		overflow_;
	/* The slots a change has claimed or counted a page out of. */
	std::uint64_t touched_{};
	/* Overflow pages reserved and not yet filled. */
	std::size_t unfilled_{};
};

} // namespace uml::memory
