// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Tiered execution of an x86-64 address space: the gadget engine runs it
 * until it has retired a threshold of instructions, and the JIT runs it from
 * then on. A process that exits first never pays the JIT's compile cost, and
 * one that stays pays it once.
 *
 * Neither engine keeps state of its own across a dispatch: both read and
 * write the vCPU's architectural state and the address space. So the engine
 * can change between dispatches, provided no dispatch of the other engine is
 * still running in the address space. A store of the JIT publishes a code
 * change only for a page its own registry holds, and a gadget translation
 * holds nothing there, so a JIT store could leave a gadget translation of the
 * page stale. The handover therefore drains: once the address space crosses
 * its threshold no dispatch of it starts on the gadgets, each one running
 * leaves, and the last to leave opens the JIT. Dispatches that begin
 * meanwhile wait, which costs one gadget dispatch's length, at most, once in
 * the life of an address space. */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace uml::runtime {

class X86TierState final {
public:
	enum class Engine : std::uint8_t {
		gadgets,
		jit,
	};

	/* Joins the dispatch to an engine. A dispatch on the gadgets stays
	 * counted until leave() or the guard's destructor; one on the JIT counts
	 * for nothing. `abandon` is polled while the handover is draining and
	 * ends the wait with `abandoned()` set, for a run that is being
	 * stopped. */
	class Entry final {
	public:
		Entry() noexcept = default;
		Entry(const Entry &) = delete;
		Entry &operator=(const Entry &) = delete;
		Entry(Entry &&other) noexcept
			: state_{std::exchange(other.state_, nullptr)},
			  engine_{other.engine_}, abandoned_{other.abandoned_}
		{
		}
		Entry &operator=(Entry &&other) noexcept
		{
			if (this != &other) {
				leave(0);
				state_ = std::exchange(other.state_, nullptr);
				engine_ = other.engine_;
				abandoned_ = other.abandoned_;
			}
			return *this;
		}
		~Entry() { leave(0); }

		[[nodiscard]] Engine engine() const noexcept { return engine_; }
		[[nodiscard]] bool abandoned() const noexcept { return abandoned_; }

		/* Ends a gadget dispatch that retired `retired` instructions. */
		void leave(std::uint64_t retired) noexcept
		{
			if (state_ != nullptr)
				std::exchange(state_, nullptr)->leave(retired);
		}

	private:
		friend class X86TierState;

		X86TierState *state_{};
		Engine engine_{Engine::jit};
		bool abandoned_{};
	};

	explicit X86TierState(std::uint64_t threshold) noexcept
		: threshold_{threshold}
	{
	}

	template<typename Abandon>
	[[nodiscard]] Entry enter(Abandon &&abandon) noexcept
	{
		Entry entry;
		for (;;) {
			const auto phase = phase_.load(std::memory_order_acquire);
			if (phase == Phase::jit)
				return entry;
			if (phase == Phase::gadgets) {
				active_.fetch_add(1, std::memory_order_acq_rel);
				if (phase_.load(std::memory_order_acquire) ==
				    Phase::gadgets) {
					entry.state_ = this;
					entry.engine_ = Engine::gadgets;
					return entry;
				}
				/* The handover began between the two reads. */
				release();
				continue;
			}
			if (abandon()) {
				entry.abandoned_ = true;
				return entry;
			}
			std::this_thread::yield();
		}
	}

	[[nodiscard]] Entry enter() noexcept
	{
		return enter([]() noexcept { return false; });
	}

	[[nodiscard]] bool on_jit() const noexcept
	{
		return phase_.load(std::memory_order_acquire) == Phase::jit;
	}

	[[nodiscard]] std::uint64_t retired() const noexcept
	{
		return retired_.load(std::memory_order_relaxed);
	}

private:
	enum class Phase : std::uint32_t {
		gadgets,
		draining,
		jit,
	};

	void leave(std::uint64_t retired) noexcept
	{
		if (retired != 0 &&
		    retired_.fetch_add(retired, std::memory_order_relaxed) +
			    retired >= threshold_) {
			auto expected = Phase::gadgets;
			(void)phase_.compare_exchange_strong(expected, Phase::draining,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
		release();
	}

	/* Returns one gadget count, and opens the JIT when that was the last
	 * one of a draining handover. */
	void release() noexcept
	{
		if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
		    phase_.load(std::memory_order_acquire) == Phase::draining) {
			auto expected = Phase::draining;
			(void)phase_.compare_exchange_strong(expected, Phase::jit,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
	}

	const std::uint64_t threshold_;
	std::atomic<std::uint64_t> retired_{};
	std::atomic<std::uint32_t> active_{};
	std::atomic<Phase> phase_{Phase::gadgets};
};

/* The state of each address space the run has seen. A generation never comes
 * back, so a dead one's state is dropped once the table is large; a state
 * still held by a vCPU stays alive through it. */
class X86TierTable final {
public:
	using Live = bool (*)(unsigned long generation) noexcept;

	X86TierTable(std::uint64_t threshold, Live live) noexcept
		: threshold_{threshold}, live_{live}
	{
	}

	[[nodiscard]] std::uint64_t threshold() const noexcept
	{
		return threshold_;
	}

	/* The state of `generation`, made on first use; null when the table
	 * cannot hold one. */
	[[nodiscard]] std::shared_ptr<X86TierState> acquire(
		unsigned long generation) noexcept
	{
		try {
			const std::lock_guard lock{mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end())
				return found->second;
			if (states_.size() >= prune_at_ && live_ != nullptr) {
				std::erase_if(states_, [&](const auto &entry) {
					return !live_(entry.first);
				});
				prune_at_ = std::max<std::size_t>(
					minimum_prune, states_.size() * 2);
			}
			auto state = std::make_shared<X86TierState>(threshold_);
			states_.emplace(generation, state);
			return state;
		} catch (...) {
			return nullptr;
		}
	}

	[[nodiscard]] std::size_t size() const
	{
		const std::lock_guard lock{mutex_};
		return states_.size();
	}

private:
	static constexpr std::size_t minimum_prune = 1024;

	const std::uint64_t threshold_;
	const Live live_;
	mutable std::mutex mutex_;
	std::unordered_map<unsigned long, std::shared_ptr<X86TierState>> states_;
	std::size_t prune_at_{minimum_prune};
};

} // namespace uml::runtime
