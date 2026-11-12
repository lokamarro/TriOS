// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace uml::testing {

/* Ends the process with a diagnosis when the code it watches runs past
 * `limit`, so a hang fails with a message before CTest's own timeout kills a
 * silent binary. Construct it around the call that may hang, or at the top of
 * `main` to bound the whole binary; destroying it cancels the watch. The
 * thread is the test's own, never a kernel thread of the runtime. */
class Watchdog final {
public:
	Watchdog(std::string_view what, std::chrono::milliseconds limit)
		: thread_{[this, what = std::string{what}, limit] {
			std::unique_lock lock{mutex_};
			if (condition_.wait_for(lock, limit,
				    [this] { return done_; }))
				return;
			std::fprintf(stderr,
				"FAIL: %s did not return within %g s\n",
				what.c_str(), limit.count() / 1000.0);
			std::fflush(stderr);
			std::_Exit(EXIT_FAILURE);
		}}
	{
	}

	~Watchdog()
	{
		{
			std::lock_guard lock{mutex_};
			done_ = true;
		}
		condition_.notify_all();
		thread_.join();
	}

	Watchdog(const Watchdog &) = delete;
	Watchdog &operator=(const Watchdog &) = delete;

private:
	std::mutex mutex_;
	std::condition_variable condition_;
	bool done_{};
	std::thread thread_;
};

} // namespace uml::testing
