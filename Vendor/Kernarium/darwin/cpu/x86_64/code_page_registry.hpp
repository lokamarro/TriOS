// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/software_tlb.hpp"
#include "memory/mmu/word_shared_mutex.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace uml::cpu::x86_64 {

/* Which guest pages hold a cached translation in any executor of one address
 * space.
 *
 * Linux marks every data page of a READ_IMPLIES_EXEC process executable, so
 * "this page may hold code" is true for the stack and the heap, and a store
 * that publishes on that predicate publishes on almost every store. What a
 * publication protects is a cached translation, and only the executors know
 * where those are. Every executor running the address space reports the pages
 * its cache covers here; the miss handler tags a write translation for
 * publication only when the page is on the list.
 *
 * The list is shared between vCPU threads, so two things keep it exact.
 * Adding a page retires every participant's write translation for it and
 * bumps a change count that the miss handler re-reads after filling, so no
 * untagged translation can outlive the moment the page gained code. And a
 * participant that is inside the engine may already have translated a store
 * it has not performed, so the adder waits until every such participant has
 * left the engine once -- bounded by the run's instruction window -- before
 * the translation reads the page's bytes.
 *
 * Membership is exact: a count per page. A bit per hashed page in front of it
 * answers "no" with one load, which is the answer for almost every page a
 * guest writes, and only a set bit is confirmed against the counts. The bits
 * alone are not enough, because the hash keeps neighbours adjacent and a run of
 * code pages shadows a run of data pages the same length: when that run was a
 * thread's stack, every block that pushed published a code change, the
 * sequence moved faster than any other vCPU could validate a cached block
 * against it, and all of them translated every block again for as long as the
 * process lived. Where a stack lands is up to the guest's address-space
 * randomization, so one run in ten lost nine tenths of its speed.
 *
 * Every executor counts a page in when it caches a translation there and out
 * when it drops one, and a guest whose code outgrows the caches does both
 * hundreds of thousands of times a second across its vCPUs. The counts are
 * therefore guarded in stripes by a lock that never sleeps; behind one
 * std::mutex the vCPUs of a multi-core compile spent a tenth of their busy
 * time queued in the kernel for it.
 */
class CodePageRegistry final {
public:
	struct Participant final {
		NativeTlbEntry *write_translations{};
		/* Odd while the participant is inside the engine. */
		std::atomic<std::uint64_t> engine_epoch{0};
	};
	/* Retires the write translation a participant whose software TLB has
	 * another layout holds for `page`, if it holds one. Any thread may call
	 * it, while the participant runs. */
	using RetireWriteTranslation = void (*)(
		void *context, std::uint64_t page) noexcept;

	/* One registry per guest address-space generation, shared for as long as
	 * any executor holds it. */
	[[nodiscard]] static std::shared_ptr<CodePageRegistry> acquire(
		std::uint64_t generation);

	CodePageRegistry();
	~CodePageRegistry();
	CodePageRegistry(const CodePageRegistry &) = delete;
	CodePageRegistry &operator=(const CodePageRegistry &) = delete;

	void join(Participant &participant);
	/* A participant that leaves `write_translations` null retires its own
	 * entries through `retire`. */
	void join(Participant &participant, RetireWriteTranslation retire,
		void *context);
	void leave(Participant &participant) noexcept;

	[[nodiscard]] bool holds(std::uint64_t address) const noexcept
	{
		const auto bit = bit_index(address);
		if ((bits_[bit >> 6U].load(std::memory_order_relaxed) &
			    (std::uint64_t{1} << (bit & 63U))) == 0)
			return false;
		return holds_exactly(address, bit);
	}
	[[nodiscard]] std::uint64_t changes() const noexcept
	{
		return changes_.load(std::memory_order_acquire);
	}

	/* Called by the executor that is about to translate `page`, before it
	 * reads the page's bytes. */
	void add(std::uint64_t page, const Participant *self) noexcept;
	void remove(std::uint64_t page) noexcept;

private:
	static constexpr unsigned bit_count_log2 = 18;
	static constexpr std::size_t bit_count = std::size_t{1} << bit_count_log2;

	[[nodiscard]] static std::size_t bit_index(std::uint64_t address) noexcept
	{
		const auto page = address >> 12U;
		return static_cast<std::size_t>(
			(page ^ (page >> bit_count_log2)) & (bit_count - 1U));
	}
	[[nodiscard]] bool holds_exactly(
		std::uint64_t address, std::size_t bit) const noexcept;
	void wait_for_participants(const Participant *self) noexcept;
	void retire_write_translations(std::uint64_t page) noexcept;

	/* Neighbouring pages hash to neighbouring bits, so they land in
	 * different stripes. A stripe guards the page counts, the bit counts and
	 * the membership bits of its bits; it is taken before
	 * participants_mutex_. A page that lost its last holder keeps its entry
	 * at zero, so a cache that keeps replacing one translation does not keep
	 * allocating one. */
	static constexpr std::size_t stripe_count = 64;
	struct alignas(64) Stripe final {
		mutable ::uml::memory::WordSharedMutex mutex;
		std::unordered_map<std::uint64_t, std::uint32_t> pages;
	};
	[[nodiscard]] Stripe &stripe_for(std::size_t bit) noexcept
	{
		return stripes_[bit & (stripe_count - 1U)];
	}
	[[nodiscard]] const Stripe &stripe_for(std::size_t bit) const noexcept
	{
		return stripes_[bit & (stripe_count - 1U)];
	}

	std::array<Stripe, stripe_count> stripes_;
	::uml::memory::WordSharedMutex participants_mutex_;
	std::vector<Participant *> participants_;
	std::unique_ptr<std::atomic<std::uint64_t>[]> bits_;
	std::unique_ptr<std::uint8_t[]> counts_;
	std::atomic<std::uint64_t> changes_{0};
	/* A page count could not be allocated, so the bits answer alone again. */
	std::atomic<bool> inexact_{false};
	/* The generation whose shared table entry this registry owns; empty for
	 * a registry constructed directly. */
	std::optional<std::uint64_t> generation_;
	/* The participants that retire their own entries, guarded by
	 * participants_mutex_ like the list above. */
	struct Retirer final {
		Participant *participant{};
		RetireWriteTranslation retire{};
		void *context{};
	};
	std::vector<Retirer> retirers_;
};

} // namespace uml::cpu::x86_64
