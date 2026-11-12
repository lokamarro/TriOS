// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <time.h>
#include <utility>

namespace uml::testing {

/* A host thread under a name that spends a given CPU time and then waits,
 * without using any more, until it is let go. A test that needs CPU time to
 * add up exactly, as a guest's vCPU carriers' do, gets it from the thread's
 * own clock, whatever else the host is doing. An empty name leaves the thread
 * unnamed. */
class NamedSpinner final {
public:
	NamedSpinner(std::string name, std::chrono::milliseconds spend)
		: thread_ {[this, name = std::move(name), spend] {
			  if (!name.empty())
				  (void)::pthread_setname_np(name.c_str());
			  const auto target = threadCpuNanoseconds() +
				  static_cast<std::uint64_t>(spend.count()) * 1000000ULL;
			  while (threadCpuNanoseconds() < target) {
			  }
			  used_.store(threadCpuNanoseconds(),
				  std::memory_order_release);
			  std::unique_lock lock {mutex_};
			  spent_ = true;
			  changed_.notify_all();
			  changed_.wait(lock, [this] { return released_; });
		  }}
	{
	}

	NamedSpinner(const NamedSpinner &) = delete;
	NamedSpinner &operator=(const NamedSpinner &) = delete;

	~NamedSpinner() { release(); }

	/* The CPU time it used once it had spent it, in nanoseconds. */
	[[nodiscard]] std::uint64_t awaitSpent()
	{
		std::unique_lock lock {mutex_};
		require(changed_.wait_for(lock, std::chrono::seconds {30},
				[this] { return spent_; }),
			"the thread never finished spending its time");
		return used_.load(std::memory_order_acquire);
	}

	void release()
	{
		{
			const std::lock_guard lock {mutex_};
			released_ = true;
		}
		changed_.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

private:
	[[nodiscard]] static std::uint64_t threadCpuNanoseconds()
	{
		timespec time {};
		require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0,
			"reading the thread's CPU time");
		return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
			static_cast<std::uint64_t>(time.tv_nsec);
	}

	std::mutex mutex_;
	std::condition_variable changed_;
	bool spent_ {};
	bool released_ {};
	std::atomic<std::uint64_t> used_ {};
	std::thread thread_;
};

} // namespace uml::testing
