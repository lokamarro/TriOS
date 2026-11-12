// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <thread>

namespace uml::memory {

/* Returns the object `published` points to, creating it with `create` on first
 * use. The object lives for the whole process and is never destroyed, so a
 * static destructor or a thread exit that runs after exit() has begun can
 * still reach it. `create` returns the new object, or nullptr when it cannot
 * make one; that result is not remembered, so a transient allocation failure
 * is retried on the next call. Only one `create` runs at a time, and whatever
 * it does before returning happens before any caller sees the object.
 */
template<typename T, typename Create>
[[nodiscard]] T *process_singleton(std::atomic<T *> &published,
	std::atomic_flag &initializing, Create &&create) noexcept
{
	if (auto *object = published.load(std::memory_order_acquire);
	    object != nullptr)
		return object;
	while (initializing.test_and_set(std::memory_order_acquire))
		std::this_thread::yield();
	struct Unlock final {
		std::atomic_flag &flag;
		~Unlock() { flag.clear(std::memory_order_release); }
	} unlock{initializing};
	if (auto *object = published.load(std::memory_order_acquire);
	    object != nullptr)
		return object;
	T *object = create();
	if (object != nullptr)
		published.store(object, std::memory_order_release);
	return object;
}

} // namespace uml::memory
