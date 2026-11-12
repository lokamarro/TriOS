// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace uml::cpu::gadget_cache {

/* A process-wide byte budget for what the gadget caches keep per address
 * space.
 *
 * Slots bound how many translations a cache holds, not how large they are: at
 * 262,144 slots, every slot holding a block of the largest arena class would be
 * 4 GiB per vCPU, and a shared space's link records grow with every target it
 * ever linked to. So the storage each address space keeps -- its translations
 * and whatever holds them, its link and indirect records, its target tables --
 * is charged to an Account, one per address space as the engine keeps it (an
 * arm64 space, shared by the executors running it or private to one; an x86
 * executor's own record of an address space), and the accounts add up to one
 * figure for the process. A translation several executors share is charged
 * once, to the space that holds it. The slots are not charged: the slot count
 * fixes their bytes.
 *
 * The budget caps the accounts that are open. A store that finds them above it
 * calls relieve(), which asks the largest open accounts to shed until they hold
 * at most seven eighths of the budget. An account asked to shed tells its
 * engine (Notify), and every executor holding that address space lets go of it
 * at its next entry into translated code, as it lets go of a renewed space;
 * the storage goes with the last of them, and the address space starts afresh
 * in a new account when it runs again. Until then a shedding account's bytes,
 * whatever it charges or refunds after it was asked, are counted apart from
 * the open ones: every charge is attributed to one figure or the other exactly
 * once. Every store past the budget relieves it in turn, counting the open
 * accounts again under the ledger's lock, so the open accounts stay within the
 * budget but for the stores in flight, one per storing vCPU, and a closed
 * space that an idle vCPU still holds -- part of what was open when that vCPU
 * went idle -- leaves the whole at most twice the budget.
 *
 * The emulator sets the budget before any executor exists
 * (derive_budget()); a process that never does keeps fallback_budget.
 */

class Account;
struct LedgerState;
inline void relieve() noexcept;
[[nodiscard]] inline LedgerState ledger_state();

namespace detail {

/* Accounts are linked through themselves, so opening one never allocates. */
struct Ledger final {
	std::mutex mutex;
	Account *first{};
	std::atomic<std::size_t> budget;
	/* The bytes of the open accounts, and of those asked to shed. Each
	 * charge lands in exactly one of them (Account::charge()), so they are
	 * exact but for the charges in flight. */
	std::atomic<std::size_t> open{};
	std::atomic<std::size_t> shedding{};
	std::atomic<std::uint64_t> reliefs{};
	std::atomic<std::uint64_t> shed_accounts{};
	std::atomic<std::size_t> peak_open{};
};

} // namespace detail

/* What a process that never chose a budget keeps: a unit test, or an embedding
 * that runs executors without the emulator. */
inline constexpr std::size_t fallback_budget = std::size_t{1} << 30;
/* derive_budget() never goes below this for a guest's memory: a full cache of
 * one vCPU at the default slot count, of the body sizes Geekbench measures,
 * with its link records, is about 100 MiB. */
inline constexpr std::size_t minimum_budget = std::size_t{128} << 20;
/* The share of a host's process memory limit a budget may take, as a divisor
 * of the limit. */
inline constexpr std::size_t limit_divisor = 8;
/* The share of guest memory a budget takes, as a divisor. */
inline constexpr std::size_t guest_memory_divisor = 4;

/* Never destroyed, under the exit contract in docs/architecture.md: carriers may
 * still store, and lock the ledger to relieve it, while exit() runs. */
[[nodiscard]] inline detail::Ledger &ledger() noexcept
{
	[[clang::no_destroy]] static detail::Ledger instance{
		.budget = fallback_budget};
	return instance;
}

/* The budget for a guest of `guest_memory` bytes on a host that limits the
 * process to `memory_limit` bytes, zero for none: a quarter of guest memory,
 * at least minimum_budget, and at most an eighth of the limit.
 *
 * At a quarter of eight GiB, the memory the benchmark guests run with, the
 * largest figure measured (multi-core Geekbench on four x86 vCPUs, whose
 * executors keep their translations to themselves) is a seventh of the budget;
 * on the iPad mini 6 the limit (2.8 GiB) gives 358 MiB where a 2 GiB guest
 * would give 512. */
[[nodiscard]] constexpr std::size_t derive_budget(std::size_t guest_memory,
	std::size_t memory_limit) noexcept
{
	auto budget = std::max(minimum_budget,
		guest_memory / guest_memory_divisor);
	if (memory_limit != 0)
		budget = std::min(budget, std::max<std::size_t>(
			memory_limit / limit_divisor, std::size_t{16} << 20));
	return budget;
}

/* Set the process's budget. A run sets it once, before its first executor;
 * tests set it as they need. */
inline void set_budget(std::size_t bytes) noexcept
{
	ledger().budget.store(std::max<std::size_t>(bytes, 1),
		std::memory_order_relaxed);
}

[[nodiscard]] inline std::size_t budget() noexcept
{
	return ledger().budget.load(std::memory_order_relaxed);
}

/* Whether the open accounts hold more than the budget: what every store asks,
 * two loads. */
[[nodiscard]] inline bool over_budget() noexcept
{
	auto &book = ledger();
	return book.open.load(std::memory_order_relaxed) >
		book.budget.load(std::memory_order_relaxed);
}

/* One address space's storage, charged to the process. The engine charges it
 * as that storage grows and shrinks and destroys it with the address space,
 * which refunds what is left. `notify` is called with `owner`, under the
 * ledger's lock, when the account must shed: it may only set what the
 * executors holding the address space read at their next entry, and take no
 * lock but ones the ledger's is always taken before. The owner keeps
 * everything `notify` touches alive until the account is destroyed, which
 * waits for a call in progress. */
class Account final {
public:
	using Notify = void (*)(void *owner) noexcept;

	Account(Notify notify, void *owner) noexcept
		: notify_{notify}, owner_{owner}
	{
		auto &book = ledger();
		const std::lock_guard guard{book.mutex};
		next_ = book.first;
		if (next_ != nullptr)
			next_->previous_ = this;
		book.first = this;
	}

	~Account()
	{
		auto &book = ledger();
		const std::lock_guard guard{book.mutex};
		if (previous_ != nullptr)
			previous_->next_ = next_;
		else
			book.first = next_;
		if (next_ != nullptr)
			next_->previous_ = previous_;
		const auto state = state_.load(std::memory_order_relaxed);
		figure(state).fetch_sub(state & bytes_mask,
			std::memory_order_relaxed);
	}

	Account(const Account &) = delete;
	Account &operator=(const Account &) = delete;

	/* The address space's storage grew by `delta` bytes, or shrank. The
	 * change counts in the open figure or the shedding one as the account
	 * stood when it was made, which one atomic step decides: a change made
	 * before the account was asked to shed moves with it when it is asked,
	 * and one made after counts as shedding. An account never holds less
	 * than nothing, so a refund never reaches the flag. */
	void charge(std::ptrdiff_t delta) noexcept
	{
		if (delta == 0)
			return;
		const auto bytes = static_cast<std::uint64_t>(delta);
		const auto before = state_.fetch_add(bytes,
			std::memory_order_relaxed);
		figure(before).fetch_add(static_cast<std::size_t>(bytes),
			std::memory_order_relaxed);
	}

	[[nodiscard]] std::size_t bytes() const noexcept
	{
		return static_cast<std::size_t>(
			state_.load(std::memory_order_relaxed) & bytes_mask);
	}

	/* Whether the address space was asked to shed: its executors let go
	 * of it at their next entry, and none takes it up again. */
	[[nodiscard]] bool shedding() const noexcept
	{
		return (state_.load(std::memory_order_relaxed) &
			shedding_flag) != 0;
	}

private:
	friend void relieve() noexcept;
	friend LedgerState ledger_state();

	static constexpr std::uint64_t shedding_flag = std::uint64_t{1} << 63;
	static constexpr std::uint64_t bytes_mask = shedding_flag - 1;

	/* The ledger's figure a change to an account in `state` counts in. */
	[[nodiscard]] static std::atomic<std::size_t> &figure(
		std::uint64_t state) noexcept
	{
		auto &book = ledger();
		return (state & shedding_flag) != 0 ? book.shedding : book.open;
	}

	/* Ask the account to shed, under the ledger's lock, and move what it
	 * held when asked from the open figure to the shedding one; return
	 * that. A charge made after counts as shedding by itself. */
	std::size_t start_shedding() noexcept
	{
		const auto before = state_.fetch_or(shedding_flag,
			std::memory_order_relaxed);
		const auto bytes = static_cast<std::size_t>(before & bytes_mask);
		auto &book = ledger();
		book.open.fetch_sub(bytes, std::memory_order_relaxed);
		book.shedding.fetch_add(bytes, std::memory_order_relaxed);
		return bytes;
	}

	Notify notify_;
	void *owner_;
	/* What the account holds, with shedding_flag once it was asked to. */
	std::atomic<std::uint64_t> state_{};
	Account *previous_{};
	Account *next_{};
};

/* Ask the largest open accounts to shed until the open ones hold at most seven
 * eighths of the budget, when they hold more than all of it. Every caller
 * takes its turn, however the lock is held: one that went on storing while
 * another held it, for an account opening or going, a census or a relief, could
 * take the open accounts past the budget by as much as it stored meanwhile. A
 * caller that waited counts again and finds the budget kept, or relieves it
 * once more. No one holding the ledger's lock waits for anything but the locks
 * taken after it, so the wait is short, and it comes only past the budget. */
inline void relieve() noexcept
{
	auto &book = ledger();
	const std::lock_guard guard{book.mutex};
	/* The open accounts, counted afresh: what they hold, not the figure
	 * a charge in flight may not have reached yet. */
	std::size_t open = 0;
	for (auto *account = book.first; account != nullptr;
	     account = account->next_)
		if (!account->shedding())
			open += account->bytes();
	const auto budget = book.budget.load(std::memory_order_relaxed);
	if (open > book.peak_open.load(std::memory_order_relaxed))
		book.peak_open.store(open, std::memory_order_relaxed);
	if (open <= budget)
		return;
	const auto target = budget - budget / 8;
	bool relieved = false;
	while (open > target) {
		Account *largest = nullptr;
		for (auto *account = book.first; account != nullptr;
		     account = account->next_)
			if (!account->shedding() &&
			    (largest == nullptr ||
			     account->bytes() > largest->bytes()))
				largest = account;
		if (largest == nullptr || largest->bytes() == 0)
			break;
		const auto shed = largest->start_shedding();
		open = open > shed ? open - shed : 0;
		book.shed_accounts.fetch_add(1, std::memory_order_relaxed);
		relieved = true;
		largest->notify_(largest->owner_);
	}
	if (relieved)
		book.reliefs.fetch_add(1, std::memory_order_relaxed);
}

/* What the ledger holds, for diagnostics and tests. */
struct LedgerState final {
	std::size_t budget;
	/* Every account's bytes, and those of the accounts asked to shed. */
	std::size_t total;
	std::size_t shedding;
	/* The accounts, and the most the open ones held when relieve()
	 * counted them, which it does only past the budget. */
	std::size_t accounts;
	std::size_t peak_open;
	/* relieve() calls that asked an account to shed, and the accounts. */
	std::uint64_t reliefs;
	std::uint64_t shed_accounts;
};

/* Counted under the ledger's lock, so the figures agree with one another;
 * for a census or a test, not for a store. */
[[nodiscard]] inline LedgerState ledger_state()
{
	auto &book = ledger();
	const std::lock_guard guard{book.mutex};
	LedgerState state{
		.budget = book.budget.load(std::memory_order_relaxed),
		.total = 0,
		.shedding = 0,
		.accounts = 0,
		.peak_open = book.peak_open.load(std::memory_order_relaxed),
		.reliefs = book.reliefs.load(std::memory_order_relaxed),
		.shed_accounts = book.shed_accounts.load(
			std::memory_order_relaxed),
	};
	for (auto *account = book.first; account != nullptr;
	     account = account->next_) {
		const auto bytes = account->bytes();
		state.total += bytes;
		if (account->shedding())
			state.shedding += bytes;
		++state.accounts;
	}
	return state;
}

/* Charge the change in a holder's bytes across a scope to `account`, null for
 * none, whether the scope finishes or throws. */
template <typename Measure>
class ChargeScope final {
public:
	ChargeScope(Account *account, Measure measure) noexcept
		: account_{account}, measure_{measure},
		  before_{account != nullptr ? measure() : 0}
	{}
	~ChargeScope()
	{
		if (account_ != nullptr)
			account_->charge(static_cast<std::ptrdiff_t>(measure_()) -
				static_cast<std::ptrdiff_t>(before_));
	}
	ChargeScope(const ChargeScope &) = delete;
	ChargeScope &operator=(const ChargeScope &) = delete;

private:
	Account *account_;
	Measure measure_;
	std::size_t before_;
};

} // namespace uml::cpu::gadget_cache
