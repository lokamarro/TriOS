// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../domain_heap.hpp"

#include "page_index.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>
#include <utility>

namespace uml::memory {

/* A generation's page translations as a radix table of atomic words: the
 * generation's only record of what it maps, and what the software-TLB fill of
 * an entered execution context reads.
 *
 * The fill is the hottest reader of a generation's mappings. Through a hash
 * map of its pages it took the reader lock, a cache line every vCPU writes,
 * and then missed on a hash node of its own for every page, so a guest spent
 * most of each fill waiting for those two lines. A leaf here holds the words of
 * 64 neighbouring pages, 512 bytes, and is read without a lock. A guest maps
 * in short runs of two or three pages, so one word per page is also the
 * smallest record that does not charge a page for its neighbours.
 *
 * A mapped page's word is never 0. Its layout:
 *
 *   bits 0-2    readable, writable, executable, as the fill judges them
 *               (executable is PROT_EXEC on a guest-code page);
 *   bit 3       PROT_EXEC as mapped;
 *   bit 4       the page holds guest code;
 *   bit 5       the guest may write the page;
 *   bits 6-11   the backing slot of the page, 0-62, or 63 for the
 *               generation's overflow table;
 *   bits 12-63  the host address of the page.
 *
 * The page's offset in its backing is its host address less the backing's
 * base, so with the backing and descriptor its slot names a word is the whole
 * mapping. The fill reads the address and the first three bits only.
 *
 * Only an entered context reads without a lock, which is what makes the
 * missing lock sound: everything that removes a page or changes one quiesces
 * execution first, so it can never run beside such a reader, and a mapping
 * added beside one only fills a word nobody can have read as present. A word
 * is absent, never stale. Every other reader holds the generation's mapping
 * lock shared and every writer holds it exclusive.
 *
 * Absence is authoritative, so a writer reserves before it commits. reserve()
 * creates the nodes a page's word will live in, the only step that allocates
 * and so the only one that can fail; store() then writes words and allocates
 * nothing. A leaf whose last word was cleared is freed by trim(), which
 * quiescence makes safe. The nodes above a leaf stay until clear(), and so
 * does a leaf that reserve() created and no word filled, since an additive
 * change does not quiesce and a reader may hold the leaf.
 *
 * An entered context never fills a page at or above the guest address limit
 * (it asks the general path), so the few such pages, native-domain addresses
 * given to the static MMU directly, are kept in a small table of their own
 * that is only read under the lock.
 */
class FillIndex final {
public:
	static constexpr std::uintptr_t readable = 1;
	static constexpr std::uintptr_t writable = 2;
	static constexpr std::uintptr_t executable = 4;
	static constexpr std::uintptr_t permissions =
		readable | writable | executable;
	/* PROT_EXEC as mapped, whether or not the page holds guest code. */
	static constexpr std::uintptr_t exec_mapped = 8;
	static constexpr std::uintptr_t guest_code_page = 16;
	static constexpr std::uintptr_t guest_writable_page = 32;
	static constexpr unsigned slot_shift = 6;
	static constexpr unsigned slot_bits = 6;
	/* Slots 0 to 62 name a backing; this one sends a reader to the overflow
	 * table. */
	static constexpr unsigned overflow_slot = (1U << slot_bits) - 1U;
	static constexpr std::uintptr_t flag_mask = (std::uintptr_t{1} << 12) - 1U;
	static constexpr std::uintptr_t address_mask = ~flag_mask;
	static_assert(DARWIN_UML_GUEST_PAGE_SHIFT >= 12,
		"the flags live in the low bits of a page's host address");

	/* The parts of a word. `host` is the host address of the page. */
	struct Fields final {
		std::uintptr_t host;
		bool read;
		bool write;
		bool exec;
		bool guest_code;
		bool guest_writable;
		unsigned slot;
	};

	/* Whether `host` can be the address of a mapped page: not null, and clear
	 * of the bits a word keeps its flags in. */
	[[nodiscard]] static constexpr bool representable(
		std::uintptr_t host) noexcept
	{
		return host != 0 && (host & flag_mask) == 0;
	}

	[[nodiscard]] static constexpr std::uintptr_t encode(
		const Fields &fields) noexcept
	{
		return fields.host |
			(fields.read ? readable : 0) |
			(fields.write ? writable : 0) |
			(fields.exec && fields.guest_code ? executable : 0) |
			(fields.exec ? exec_mapped : 0) |
			(fields.guest_code ? guest_code_page : 0) |
			(fields.guest_writable ? guest_writable_page : 0) |
			(static_cast<std::uintptr_t>(fields.slot) << slot_shift);
	}

	[[nodiscard]] static constexpr Fields decode(
		std::uintptr_t word) noexcept
	{
		return {word & address_mask,
			(word & readable) != 0,
			(word & writable) != 0,
			(word & exec_mapped) != 0,
			(word & guest_code_page) != 0,
			(word & guest_writable_page) != 0,
			static_cast<unsigned>(word >> slot_shift) & overflow_slot};
	}

	/* Which leaf `page` lies in: pages with the same number share one. */
	[[nodiscard]] static constexpr std::uint64_t leaf_of(
		std::uint64_t page) noexcept
	{
		return page >> (page_shift + leaf_bits);
	}

	FillIndex() noexcept = default;
	/* Nodes and leaves come from `heap`, or from the global allocator when
	 * there is none. A heap must outlive the index. */
	explicit FillIndex(DomainHeap *heap) noexcept : heap_{heap} {}
	FillIndex(const FillIndex &) = delete;
	FillIndex &operator=(const FillIndex &) = delete;
	~FillIndex() { clear(); }

	/* The word of `page` for an entered context, without a lock. A page at or
	 * above the guest limit reads as absent; lookup() finds it. */
	[[nodiscard]] std::uintptr_t find(std::uint64_t page) const noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0)
			return 0;
		const auto *upper = roots_[number >> upper_shift].load(
			std::memory_order_acquire);
		if (upper == nullptr)
			return 0;
		const auto *middle = upper->children[index(number, middle_shift)]
			.load(std::memory_order_acquire);
		if (middle == nullptr)
			return 0;
		const auto *lower = middle->children[index(number, lower_shift)]
			.load(std::memory_order_acquire);
		if (lower == nullptr)
			return 0;
		const auto *leaf = lower->children[index(number, leaf_bits)].load(
			std::memory_order_acquire);
		if (leaf == nullptr)
			return 0;
		return leaf->entries[number & leaf_mask].load(
			std::memory_order_acquire);
	}

	/* The word of `page` for a reader that holds the generation's lock. */
	[[nodiscard]] std::uintptr_t lookup(std::uint64_t page) const noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) == 0)
			return find(page);
		const auto found = high_.find(number);
		return found == high_.end() ? 0 : found->second;
	}

	/* Creates the nodes that `page`'s word lives in. False when one cannot be
	 * allocated; the nodes created before it stay. */
	[[nodiscard]] bool reserve(std::uint64_t page) noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0) {
			try {
				high_.try_emplace(number, std::uintptr_t{0});
			} catch (...) {
				return false;
			}
			return true;
		}
		auto *upper = child(roots_[number >> upper_shift], true);
		if (upper == nullptr)
			return false;
		auto *middle = child(
			upper->children[index(number, middle_shift)], true);
		if (middle == nullptr)
			return false;
		auto *lower = child(
			middle->children[index(number, lower_shift)], true);
		if (lower == nullptr)
			return false;
		return child(lower->children[index(number, leaf_bits)], true) !=
			nullptr;
	}

	/* Writes the word of `page`; 0 clears it, leaving the leaf for trim().
	 * Allocates nothing: a nonzero word needs the page reserved, and is
	 * dropped, which reads as absent, if it was not. */
	void store(std::uint64_t page, std::uintptr_t word) noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0) {
			const auto found = high_.find(number);
			if (found == high_.end())
				return;
			if (word != 0)
				found->second = word;
			else
				high_.erase(found);
			return;
		}
		auto *slot = leaf_slot(number);
		auto *leaf = slot == nullptr ?
			nullptr : slot->load(std::memory_order_relaxed);
		if (leaf == nullptr)
			return;
		leaf->entries[number & leaf_mask].store(
			word, std::memory_order_release);
	}

	/* Frees the leaf of `page` if no word in it is set. Writers only, with
	 * execution quiesced: otherwise a reader may hold the leaf. */
	void trim(std::uint64_t page) noexcept
	{
		const auto number = page >> page_shift;
		if ((number >> page_bits) != 0)
			return;
		auto *slot = leaf_slot(number);
		if (slot == nullptr)
			return;
		auto *leaf = slot->load(std::memory_order_relaxed);
		if (leaf == nullptr || !empty(*leaf))
			return;
		slot->store(nullptr, std::memory_order_relaxed);
		destroy(leaf);
	}

	/* store(page, 0) and trim(page): writers only, execution quiesced. */
	void erase(std::uint64_t page) noexcept
	{
		store(page, 0);
		trim(page);
	}

	/* Calls visit(page, word) for each mapped page in [first, end), lowest
	 * first, until it returns true; true if one did. The walk skips every
	 * subtree that holds no leaf, so its cost follows what is mapped rather
	 * than the width of the range. Pages at or above the guest limit come
	 * last, in no order. For a reader that holds the generation's lock. */
	template <typename Visit>
	bool scan(std::uint64_t first, std::uint64_t end, Visit &&visit) const
	{
		const bool stopped = walk(first, end,
			[&](std::atomic<Leaf *> &, Leaf &leaf, std::uint64_t base,
				std::size_t from, std::size_t to) {
				for (auto entry = from; entry <= to; ++entry) {
					const auto word = leaf.entries[entry].load(
						std::memory_order_acquire);
					if (word != 0 &&
					    visit(page_address(base + entry), word))
						return true;
				}
				return false;
			});
		if (stopped || high_.empty())
			return stopped;
		for (const auto &[number, word] : high_) {
			const auto page = page_address(number);
			if (page >= first && page < end && visit(page, word))
				return true;
		}
		return false;
	}

	/* Calls note(page, word) for each mapped page in [first, end) and clears
	 * it, then frees the leaves that leaves empty. Writers only, with
	 * execution quiesced. Allocates nothing. */
	template <typename Note>
	void erase_range(std::uint64_t first, std::uint64_t end, Note &&note)
	{
		(void)walk(first, end,
			[&](std::atomic<Leaf *> &slot, Leaf &leaf, std::uint64_t base,
				std::size_t from, std::size_t to) {
				for (auto entry = from; entry <= to; ++entry) {
					const auto word = leaf.entries[entry].load(
						std::memory_order_relaxed);
					if (word == 0)
						continue;
					note(page_address(base + entry), word);
					leaf.entries[entry].store(
						0, std::memory_order_release);
				}
				if (empty(leaf)) {
					slot.store(nullptr, std::memory_order_relaxed);
					destroy(&leaf);
				}
				return false;
			});
		for (auto found = high_.begin(); found != high_.end();) {
			const auto page = page_address(found->first);
			if (page >= first && page < end) {
				note(page, found->second);
				found = high_.erase(found);
			} else {
				++found;
			}
		}
	}

	/* Writers only, with execution quiesced: frees every node. */
	void clear() noexcept
	{
		for (auto &root : roots_) {
			auto *upper = root.load(std::memory_order_relaxed);
			if (upper == nullptr)
				continue;
			for (auto &middle_slot : upper->children) {
				auto *middle = middle_slot.load(std::memory_order_relaxed);
				if (middle == nullptr)
					continue;
				for (auto &lower_slot : middle->children) {
					auto *lower =
						lower_slot.load(std::memory_order_relaxed);
					if (lower == nullptr)
						continue;
					for (auto &leaf : lower->children)
						destroy(leaf.load(std::memory_order_relaxed));
					destroy(lower);
				}
				destroy(middle);
			}
			destroy(upper);
			root.store(nullptr, std::memory_order_relaxed);
		}
		high_.clear();
	}

private:
	static constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
	static constexpr unsigned leaf_bits = 6;
	static constexpr unsigned node_bits = 8;
	static constexpr std::size_t leaf_entries = std::size_t{1} << leaf_bits;
	static constexpr std::size_t node_entries = std::size_t{1} << node_bits;
	static constexpr std::uint64_t leaf_mask = leaf_entries - 1U;
	static constexpr std::uint64_t node_mask = node_entries - 1U;
	static constexpr unsigned lower_shift = leaf_bits + node_bits;
	static constexpr unsigned middle_shift = lower_shift + node_bits;
	static constexpr unsigned upper_shift = middle_shift + node_bits;
	/* Every guest address lies below the guest limit. */
	static constexpr unsigned page_bits = static_cast<unsigned>(std::bit_width(
		std::uint64_t{DARWIN_UML_GUEST_ADDRESS_LIMIT} - 1U)) - page_shift;
	static_assert(page_bits > upper_shift && page_bits <= upper_shift + 10U,
		"three nodes and a leaf below a root of at most 1024 entries");
	static constexpr std::size_t root_entries =
		std::size_t{1} << (page_bits - upper_shift);

	[[nodiscard]] static constexpr std::size_t index(
		std::uint64_t number, unsigned shift) noexcept
	{
		return static_cast<std::size_t>((number >> shift) & node_mask);
	}

	[[nodiscard]] static constexpr std::uint64_t page_address(
		std::uint64_t number) noexcept
	{
		return number << page_shift;
	}

	/* A leaf is exactly its 512 bytes of words. A count of them would push it
	 * into the next allocator size class, so a removal that wants to know
	 * whether the leaf is empty scans it instead. */
	struct Leaf final {
		std::array<std::atomic_uintptr_t, leaf_entries> entries{};
	};
	static_assert(sizeof(Leaf) == 512);
	template <typename Child>
	struct Node final {
		std::array<std::atomic<Child *>, node_entries> children{};
	};
	using Lower = Node<Leaf>;
	using Middle = Node<Lower>;
	using Upper = Node<Middle>;

	template <typename T>
	[[nodiscard]] T *make() noexcept
	{
		if (heap_ == nullptr)
			return new (std::nothrow) T{};
		try {
			return new (heap_->allocate(sizeof(T), alignof(T))) T{};
		} catch (...) {
			return nullptr;
		}
	}

	template <typename T>
	void destroy(T *node) noexcept
	{
		if (node == nullptr)
			return;
		if (heap_ == nullptr) {
			delete node;
			return;
		}
		node->~T();
		heap_->deallocate(node, sizeof(T), alignof(T));
	}

	template <typename Child>
	[[nodiscard]] Child *child(std::atomic<Child *> &slot,
		bool create) noexcept
	{
		auto *found = slot.load(std::memory_order_relaxed);
		if (found != nullptr || !create)
			return found;
		/* Published whole: a reader that sees the pointer sees zeroes. */
		found = make<Child>();
		if (found != nullptr)
			slot.store(found, std::memory_order_release);
		return found;
	}

	[[nodiscard]] static bool empty(const Leaf &leaf) noexcept
	{
		std::uintptr_t any = 0;
		for (const auto &entry : leaf.entries)
			any |= entry.load(std::memory_order_relaxed);
		return any == 0;
	}

	/* The slot that holds the leaf of page `number`, or null when a node above
	 * it does not exist. */
	[[nodiscard]] std::atomic<Leaf *> *leaf_slot(
		std::uint64_t number) const noexcept
	{
		auto *upper = roots_[number >> upper_shift].load(
			std::memory_order_relaxed);
		if (upper == nullptr)
			return nullptr;
		auto *middle = upper->children[index(number, middle_shift)].load(
			std::memory_order_relaxed);
		if (middle == nullptr)
			return nullptr;
		auto *lower = middle->children[index(number, lower_shift)].load(
			std::memory_order_relaxed);
		if (lower == nullptr)
			return nullptr;
		return &lower->children[index(number, leaf_bits)];
	}

	/* Calls visit(slot, leaf, base, from, to) for every leaf under
	 * [first, end) that exists, lowest first, until it returns true. `base` is
	 * the number of the leaf's first page and from..to the entries of it
	 * inside the range. A subtree with no node is skipped whole. */
	template <typename Visit>
	bool walk(std::uint64_t first, std::uint64_t end, Visit &&visit) const
	{
		if (end <= first)
			return false;
		const auto first_number = first >> page_shift;
		if ((first_number >> page_bits) != 0)
			return false;
		const auto last_number = std::min(
			(end - 1U) >> page_shift,
			(std::uint64_t{1} << page_bits) - 1U);
		/* The children of the node whose prefix is `parent` that the range
		 * touches, as indices into it. */
		const auto touched = [&](std::uint64_t parent, unsigned shift) {
			const auto low = parent << node_bits;
			return std::pair{
				std::max(first_number >> shift, low) - low,
				std::min(last_number >> shift, low | node_mask) - low};
		};
		for (auto root = first_number >> upper_shift;
		     root <= last_number >> upper_shift; ++root) {
			auto *upper = roots_[root].load(std::memory_order_acquire);
			if (upper == nullptr)
				continue;
			const auto [middle_from, middle_to] =
				touched(root, middle_shift);
			for (auto m = middle_from; m <= middle_to; ++m) {
				auto *middle = upper->children[m].load(
					std::memory_order_acquire);
				if (middle == nullptr)
					continue;
				const auto middle_prefix = (root << node_bits) | m;
				const auto [lower_from, lower_to] =
					touched(middle_prefix, lower_shift);
				for (auto l = lower_from; l <= lower_to; ++l) {
					auto *lower = middle->children[l].load(
						std::memory_order_acquire);
					if (lower == nullptr)
						continue;
					const auto lower_prefix =
						(middle_prefix << node_bits) | l;
					const auto [leaf_from, leaf_to] =
						touched(lower_prefix, leaf_bits);
					for (auto f = leaf_from; f <= leaf_to; ++f) {
						auto &slot = lower->children[f];
						auto *leaf = slot.load(std::memory_order_acquire);
						if (leaf == nullptr)
							continue;
						const auto base =
							((lower_prefix << node_bits) | f) << leaf_bits;
						const auto from = first_number > base ?
							first_number - base : std::uint64_t{0};
						const auto to = std::min<std::uint64_t>(
							last_number - base, leaf_mask);
						if (visit(slot, *leaf, base,
							    static_cast<std::size_t>(from),
							    static_cast<std::size_t>(to)))
							return true;
					}
				}
			}
		}
		return false;
	}

	struct NumberHash final {
		[[nodiscard]] std::size_t operator()(
			std::uint64_t number) const noexcept
		{
			return static_cast<std::size_t>(fold_page_key(number));
		}
	};

	DomainHeap *heap_{};
	std::array<std::atomic<Upper *>, root_entries> roots_{};
	/* Writers and locked readers only. */
	std::unordered_map<std::uint64_t, std::uintptr_t, NumberHash> high_;
};

} // namespace uml::memory
