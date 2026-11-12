// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * Another thread that stops this one in the kernel at random, as
 * preemption does: each stop is a kernel entry. XNU resets FPSR to
 * 0x0800009F on return from any kernel entry while a thread is in
 * streaming mode, so the tests of that contract run a long streaming loop
 * under it and check that the guest's FPSR and FPCR survive.
 */
#include "require.hpp"
#include "wait_until.hpp"

#include <mach/mach.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace uml::emulator::tests {

class KernelStops final {
public:
	explicit KernelStops(std::chrono::microseconds period =
		std::chrono::microseconds{200})
		: target_{mach_thread_self()}, period_{period},
		  thread_{[this] { interrupt(); }}
	{
		/* The run that follows is stopped only once this thread is
		 * going: a loaded host can schedule it after that run is
		 * over. */
		const auto stopped = [this] {
			return stops_.load(std::memory_order_acquire) != 0;
		};
		uml::testing::require(uml::testing::waitUntil(stopped,
				std::chrono::seconds{10}),
			"the thread that stops this one never stopped it");
	}

	KernelStops(const KernelStops &) = delete;
	KernelStops &operator=(const KernelStops &) = delete;

	~KernelStops()
	{
		stop();
		mach_port_deallocate(mach_task_self(), target_);
	}

	/* Ends the stops and returns how many there were. */
	unsigned stop()
	{
		running_.store(false, std::memory_order_relaxed);
		if (thread_.joinable())
			thread_.join();
		return stops_.load();
	}

private:
	void interrupt()
	{
		while (running_.load(std::memory_order_relaxed)) {
			if (thread_suspend(target_) == KERN_SUCCESS) {
				/* Reading the state waits until the thread has
				 * stopped, so it entered the kernel before it
				 * resumes. */
				arm_thread_state64_t state{};
				mach_msg_type_number_t count =
					ARM_THREAD_STATE64_COUNT;
				if (thread_get_state(target_, ARM_THREAD_STATE64,
					    reinterpret_cast<thread_state_t>(
						    &state),
					    &count) == KERN_SUCCESS)
					stops_.fetch_add(1,
						std::memory_order_relaxed);
				thread_resume(target_);
			}
			std::this_thread::sleep_for(period_);
		}
	}

	mach_port_t target_;
	std::chrono::microseconds period_;
	std::atomic<bool> running_{true};
	std::atomic<unsigned> stops_{0};
	std::thread thread_;
};

} // namespace uml::emulator::tests
