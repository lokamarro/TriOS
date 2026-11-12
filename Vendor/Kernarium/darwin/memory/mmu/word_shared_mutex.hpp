// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UML_WORD_SHARED_MUTEX_TSAN 1
#endif
#endif
#ifndef UML_WORD_SHARED_MUTEX_TSAN
#define UML_WORD_SHARED_MUTEX_TSAN 0
#endif

#if UML_WORD_SHARED_MUTEX_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace uml::memory {

/* A reader-writer lock for a table that vCPU threads read on a hot path and
 * that is held, on either side, for a lookup or an insertion.
 *
 * It was written for one generation's page mappings. Every software-TLB miss
 * on every vCPU thread reads them, and every guest page fault writes them, so
 * with several vCPUs in one guest process that is the most contended lock in
 * the emulator, held for a few hundred nanoseconds at a time.
 *
 * libc++'s shared_mutex is a mutex and two condition variables: lock_shared
 * and unlock_shared each take the mutex, so readers serialize on it, a reader
 * that meets a writer sleeps in the kernel for a critical section shorter than
 * the wakeup, and every unlock can notify. Eight vCPUs running multi-core
 * Navigation on 4 KiB pages spent 30 percent of their time inside it.
 *
 * This one is a word. A reader is one compare-and-swap in and one subtraction
 * out, and nobody sleeps: a waiter spins briefly and then yields, which is the
 * right trade when the holder is done long before a sleeper could be woken. A
 * waiting writer sets `pending`, which holds new readers off so that a steady
 * stream of readers cannot starve it.
 *
 * It meets the standard SharedMutex requirements, so std::shared_lock,
 * std::unique_lock and std::lock_guard work on it unchanged.
 */
class WordSharedMutex final {
public:
	WordSharedMutex() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_create(this, __tsan_mutex_not_static);
#endif
	}

	~WordSharedMutex()
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_destroy(this, __tsan_mutex_not_static);
#endif
	}

	WordSharedMutex(const WordSharedMutex &) = delete;
	WordSharedMutex &operator=(const WordSharedMutex &) = delete;

	void lock() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_lock(this, 0);
#endif
		for (unsigned attempt = 0;; ++attempt) {
			auto state = state_.load(std::memory_order_relaxed);
			if ((state & ~pending) == 0) {
				if (state_.compare_exchange_weak(state, writer,
					std::memory_order_acquire,
					std::memory_order_relaxed))
					break;
				continue;
			}
			if ((state & pending) == 0 &&
			    !state_.compare_exchange_weak(state, state | pending,
				    std::memory_order_relaxed,
				    std::memory_order_relaxed))
				continue;
			wait(attempt);
		}
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_lock(this, 0, 0);
#endif
	}

	[[nodiscard]] bool try_lock() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_lock(this, __tsan_mutex_try_lock);
#endif
		auto state = state_.load(std::memory_order_relaxed);
		const bool locked = (state & ~pending) == 0 &&
			state_.compare_exchange_strong(state, writer,
				std::memory_order_acquire, std::memory_order_relaxed);
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_lock(this, locked ? __tsan_mutex_try_lock :
			__tsan_mutex_try_lock | __tsan_mutex_try_lock_failed, 0);
#endif
		return locked;
	}

	void unlock() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_unlock(this, 0);
#endif
		/* A writer that arrived meanwhile left `pending` set; it stays. */
		state_.fetch_and(~writer, std::memory_order_release);
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_unlock(this, 0);
#endif
	}

	void lock_shared() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_lock(this, __tsan_mutex_read_lock);
#endif
		for (unsigned attempt = 0;; ++attempt) {
			auto state = state_.load(std::memory_order_relaxed);
			if ((state & (writer | pending)) == 0) {
				if (state_.compare_exchange_weak(state, state + reader,
					std::memory_order_acquire,
					std::memory_order_relaxed))
					break;
				continue;
			}
			wait(attempt);
		}
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_lock(this, __tsan_mutex_read_lock, 0);
#endif
	}

	[[nodiscard]] bool try_lock_shared() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_lock(this,
			__tsan_mutex_read_lock | __tsan_mutex_try_lock);
#endif
		auto state = state_.load(std::memory_order_relaxed);
		const bool locked = (state & (writer | pending)) == 0 &&
			state_.compare_exchange_strong(state, state + reader,
				std::memory_order_acquire, std::memory_order_relaxed);
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_lock(this, __tsan_mutex_read_lock |
			(locked ? __tsan_mutex_try_lock :
				__tsan_mutex_try_lock | __tsan_mutex_try_lock_failed), 0);
#endif
		return locked;
	}

	void unlock_shared() noexcept
	{
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_pre_unlock(this, __tsan_mutex_read_lock);
#endif
		state_.fetch_sub(reader, std::memory_order_release);
#if UML_WORD_SHARED_MUTEX_TSAN
		__tsan_mutex_post_unlock(this, __tsan_mutex_read_lock);
#endif
	}

private:
	static constexpr std::uint64_t writer = 1;
	static constexpr std::uint64_t pending = 2;
	static constexpr std::uint64_t reader = 4;

	/* The holder is normally gone within a few hundred nanoseconds. Past a
	 * short spin it has been preempted, or it is applying a large batch, and
	 * the core is better given away than burned. A writer can also wait out a
	 * reader that holds the lock for as long as a vCPU stays in its guest;
	 * that wait sleeps between its looks. */
	static void wait(unsigned attempt) noexcept
	{
		if (attempt < 64) {
#if defined(__aarch64__)
			__asm__ volatile("isb");
#endif
			return;
		}
		if (attempt < 4096) {
			std::this_thread::yield();
			return;
		}
		std::this_thread::sleep_for(std::chrono::microseconds{100});
	}

	std::atomic<std::uint64_t> state_{};
};

} // namespace uml::memory
