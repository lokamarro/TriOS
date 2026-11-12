// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_page_registry.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace uml::cpu::x86_64 {
namespace {

/* Never destroyed, so a registry released during process teardown still
 * finds its entry and its lock. */
struct Table final {
	std::mutex mutex;
	std::unordered_map<std::uint64_t, std::weak_ptr<CodePageRegistry>>
		entries;
};
Table &table()
{
	static auto *instance = new Table{};
	return *instance;
}

/* A participant leaves the engine at the latest when its run exhausts the
 * instruction window, which is microseconds; the bound only guards against a
 * carrier thread stopped inside the engine by the host. */
constexpr auto quiescence_bound = std::chrono::milliseconds{200};

} // namespace

std::shared_ptr<CodePageRegistry> CodePageRegistry::acquire(
	std::uint64_t generation)
{
	auto &shared = table();
	const std::lock_guard lock{shared.mutex};
	auto [entry, inserted] = shared.entries.try_emplace(generation);
	if (!inserted)
		if (auto existing = entry->second.lock())
			return existing;
	auto created = std::make_shared<CodePageRegistry>();
	created->generation_ = generation;
	entry->second = created;
	return created;
}

CodePageRegistry::CodePageRegistry()
	: bits_{std::make_unique<std::atomic<std::uint64_t>[]>(bit_count / 64U)},
	  counts_{std::make_unique<std::uint8_t[]>(bit_count)}
{
	for (std::size_t word = 0; word < bit_count / 64U; ++word)
		bits_[word].store(0, std::memory_order_relaxed);
	std::fill_n(counts_.get(), bit_count, std::uint8_t{0});
}

/* Generations are never reused, so the entry of a released registry would
 * otherwise stay behind for the life of the process. A registry acquired for
 * the same generation after this one expired owns the entry now and keeps it. */
CodePageRegistry::~CodePageRegistry()
{
	if (!generation_)
		return;
	auto &shared = table();
	const std::lock_guard lock{shared.mutex};
	const auto entry = shared.entries.find(*generation_);
	if (entry != shared.entries.end() && entry->second.expired())
		shared.entries.erase(entry);
}

void CodePageRegistry::join(Participant &participant)
{
	const std::lock_guard lock{participants_mutex_};
	if (std::find(participants_.begin(), participants_.end(),
		    &participant) == participants_.end())
		participants_.push_back(&participant);
}

void CodePageRegistry::join(Participant &participant,
	RetireWriteTranslation retire, void *context)
{
	const std::lock_guard lock{participants_mutex_};
	if (std::find(participants_.begin(), participants_.end(),
		    &participant) != participants_.end())
		return;
	retirers_.push_back({&participant, retire, context});
	try {
		participants_.push_back(&participant);
	} catch (...) {
		retirers_.pop_back();
		throw;
	}
}

void CodePageRegistry::leave(Participant &participant) noexcept
{
	const std::lock_guard lock{participants_mutex_};
	const auto found = std::find(
		participants_.begin(), participants_.end(), &participant);
	if (found != participants_.end())
		participants_.erase(found);
	std::erase_if(retirers_, [&](const Retirer &retirer) {
		return retirer.participant == &participant;
	});
}

void CodePageRegistry::retire_write_translations(std::uint64_t page) noexcept
{
	const auto index = native_tlb_index(page);
	const std::shared_lock lock{participants_mutex_};
	for (auto *participant : participants_) {
		if (participant->write_translations == nullptr)
			continue;
		std::atomic_ref<std::uint64_t>{
			participant->write_translations[index].write_page}
			.store(1, std::memory_order_relaxed);
	}
	for (const auto &retirer : retirers_)
		retirer.retire(retirer.context, page);
}

bool CodePageRegistry::holds_exactly(
	std::uint64_t address, std::size_t bit) const noexcept
{
	if (inexact_.load(std::memory_order_relaxed))
		return true;
	const auto &stripe = stripe_for(bit);
	const std::shared_lock lock{stripe.mutex};
	const auto found = stripe.pages.find(address & ~std::uint64_t{0xfff});
	return found != stripe.pages.end() && found->second != 0;
}

void CodePageRegistry::add(std::uint64_t page, const Participant *self) noexcept
{
	page &= ~std::uint64_t{0xfff};
	const auto bit = bit_index(page);
	auto &stripe = stripe_for(bit);
	std::unique_lock lock{stripe.mutex};
	bool gained = false;
	try {
		gained = stripe.pages.try_emplace(page, 0).first->second++ == 0;
	} catch (...) {
		/* Without its count the page is held for as long as its bit is. */
		inexact_.store(true, std::memory_order_relaxed);
		gained = true;
	}
	auto &count = counts_[bit];
	if (count != 0xff && count++ == 0)
		bits_[bit >> 6U].fetch_or(std::uint64_t{1} << (bit & 63U),
			std::memory_order_release);
	if (!gained)
		return;
	changes_.fetch_add(1, std::memory_order_acq_rel);
	/* Order the count against the retirement below: a miss handler that read
	 * the old count before this fence sees the new count after its own. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	retire_write_translations(page);
	/* Participants may be inside the engine with a store already translated
	 * through the entry just retired. Wait for each to leave once so that
	 * store lands before the translation reads the page. */
	lock.unlock();
	wait_for_participants(self);
}

void CodePageRegistry::wait_for_participants(const Participant *self) noexcept
{
	std::vector<std::pair<Participant *, std::uint64_t>> waiting;
	{
		const std::shared_lock lock{participants_mutex_};
		for (auto *participant : participants_) {
			if (participant == self)
				continue;
			const auto epoch = participant->engine_epoch.load(
				std::memory_order_acquire);
			if ((epoch & 1U) != 0)
				waiting.emplace_back(participant, epoch);
		}
	}
	if (waiting.empty())
		return;
	const auto deadline = std::chrono::steady_clock::now() + quiescence_bound;
	for (const auto &[participant, epoch] : waiting) {
		while (participant->engine_epoch.load(std::memory_order_acquire) ==
			epoch) {
			if (std::chrono::steady_clock::now() >= deadline)
				return;
			std::this_thread::yield();
		}
	}
}

void CodePageRegistry::remove(std::uint64_t page) noexcept
{
	page &= ~std::uint64_t{0xfff};
	const auto bit = bit_index(page);
	auto &stripe = stripe_for(bit);
	const std::lock_guard lock{stripe.mutex};
	bool lost = false;
	if (const auto found = stripe.pages.find(page);
	    found != stripe.pages.end() && found->second != 0)
		lost = --found->second == 0;
	auto &count = counts_[bit];
	if (count != 0 && count != 0xff && --count == 0)
		bits_[bit >> 6U].fetch_and(~(std::uint64_t{1} << (bit & 63U)),
			std::memory_order_release);
	if (!lost)
		return;
	/* A write translation tagged while the page held code would keep
	 * publishing; retire it so the next store refills it untagged. A
	 * translation retired under a store in flight only publishes once more. */
	retire_write_translations(page);
}

} // namespace uml::cpu::x86_64
