// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "kernel_threads.hpp"

#include <cstddef>
#include <cstdint>
#include <mach/mach.h>

namespace uml::runtime {

/*
 * Holds every thread that can touch guest RAM.
 *
 * Guest RAM leaves the process while a paused guest is hibernated, so any
 * thread that could still read or write it must not run until it is back: the
 * vCPU carriers, the host services (named `uml-*`) and every thread the kernel
 * created (`kernel_pthread_create`). A held thread is Mach-suspended; the
 * threads that do the hibernating, and the rest of the process, run on.
 *
 * Holding is safe against the process's own locks. A thread stopped inside the
 * allocator, a stdio stream or the dynamic loader would keep its lock for as
 * long as it is held, and the hibernation itself, or whoever logs about it,
 * would then wait on that lock forever. A thread is therefore held only when
 * neither its program counter nor any return address on its frame chain lies
 * in those libraries; otherwise it is let go and tried again. Likewise a
 * thread that is inside `kernel_pthread_create` is not held until the thread
 * it is creating has been listed, so no thread of the kernel can be started
 * and missed. A lock of the caller's own that a held thread could be inside
 * is the caller's to rule out, as the `settled` condition below. While
 * threads are held, nothing here, and nothing the caller does between `hold`
 * and `release`, may allocate or write to a stream.
 *
 * `hold` returns true only when every such thread is held and `settled` (if
 * given) has confirmed the moment. A caller whose condition can only be
 * judged with the threads stopped, such as "no commit is in flight", passes it
 * as `settled`; when it answers false everything is released and the whole
 * attempt is repeated. On false nothing is held.
 */
class ThreadFence final {
public:
	/* Evaluated with every thread held; true accepts the moment. */
	using Settled = bool (*)(void *context) noexcept;

	struct Report final {
		/* Threads held, and by which rule: named `uml-*`, listed by the
		 * kernel's registry, or both. */
		std::size_t held{};
		std::size_t named{};
		std::size_t registered{};
		/* Attempts to hold a thread that were undone because it was inside
		 * the allocator, a stream, the loader or a thread creation. */
		unsigned deferred{};
		/* Times `settled` refused. */
		unsigned unsettled{};
		/* Times a thread stayed in the middle of creating another until
		 * everything was let go so that it could finish. */
		unsigned creator_stalls{};
		unsigned passes{};
		/* Why the last `hold` failed, 0 when it did not. */
		int failure{};
	};

	/* Reads the loaded images once, so call it where allocating is fine. */
	ThreadFence() noexcept;
	~ThreadFence();
	ThreadFence(const ThreadFence &) = delete;
	ThreadFence &operator=(const ThreadFence &) = delete;

	/* `budget_ms` bounds the attempt. */
	[[nodiscard]] bool hold(unsigned budget_ms, Settled settled = nullptr,
		void *context = nullptr) noexcept;
	void release() noexcept;

	[[nodiscard]] bool holding() const noexcept { return count_ != 0; }
	[[nodiscard]] const Report &report() const noexcept { return report_; }

	/* Failure causes in Report::failure. */
	static constexpr int too_many_threads = 1;
	static constexpr int registry_overflowed = 2;
	static constexpr int out_of_time = 3;
	static constexpr int cannot_enumerate = 4;

private:
	static constexpr std::size_t capacity = 512;

	struct Held final {
		thread_act_t thread;
		/* The reference this fence owns and gives back. */
		bool owned;
		bool named;
		bool registered;
	};

	struct Range final {
		std::uintptr_t low;
		std::uintptr_t high;
	};

	struct Pass final {
		/* Threads held for the first time in this pass. */
		std::size_t acquired;
		/* Candidates seen and not held. */
		std::size_t deferred;
	};

	[[nodiscard]] Held *find(thread_act_t thread) noexcept;
	[[nodiscard]] bool avoided(std::uintptr_t address) const noexcept;
	[[nodiscard]] bool unsafe_to_hold(thread_act_t thread) const noexcept;
	/* Holds `thread` unless it is not safe to; true when it is held. */
	[[nodiscard]] bool try_hold(thread_act_t thread, bool owned, bool named,
		bool registered, Pass &pass) noexcept;
	[[nodiscard]] bool one_pass(thread_act_t self, Pass &pass) noexcept;
	/* Resumes a held thread and forgets it. */
	void unhold(Held *held) noexcept;
	void release_all() noexcept;

	static constexpr std::size_t max_ranges = 8;
	Range avoid_[max_ranges]{};
	std::size_t avoid_count_{};
	KernelThreadSnapshot registry_{};
	Held held_[capacity];
	std::size_t count_{};
	Report report_{};
};

} // namespace uml::runtime
