// SPDX-License-Identifier: GPL-2.0-or-later
#include "thread_fence.hpp"

#include <cstring>
#include <ctime>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/arm/thread_status.h>
#include <time.h>

namespace uml::runtime {
namespace {

/* Libraries whose locks must not be held by a stopped thread: the allocator,
 * the C library's streams, the dynamic loader, the Objective-C runtime and the
 * C++ runtime's guard variables. */
constexpr const char *avoided_libraries[] = {
	"libsystem_malloc.dylib",
	"libsystem_c.dylib",
	"libdyld.dylib",
	"libobjc.A.dylib",
	"libc++abi.dylib",
};

/* A return address may carry a pointer signature in the high bits. */
constexpr std::uintptr_t address_mask = 0x0000ffffffffffffULL;

constexpr unsigned long long nanoseconds_per_millisecond = 1000000ULL;

/* How long a thread may stay in the middle of `kernel_pthread_create` before
 * the fence concludes that it is waiting for a lock a held thread owns. The
 * thread library takes its own lock to start or join a thread, and a thread
 * suspended while it owns that lock keeps a creator from ever finishing. */
constexpr unsigned creator_patience_ms = 20;

[[nodiscard]] unsigned long long now_ns() noexcept
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void pause_briefly() noexcept
{
	const timespec delay{0, 200000};
	(void)nanosleep(&delay, nullptr);
}

/* Long enough for a thread that has just been let go to run and finish. */
void pause_for_creators() noexcept
{
	const timespec delay{0, 2000000};
	(void)nanosleep(&delay, nullptr);
}

} // namespace

ThreadFence::ThreadFence() noexcept
{
	const std::uint32_t images = _dyld_image_count();
	for (std::uint32_t index = 0; index < images; ++index) {
		const char *const name = _dyld_get_image_name(index);
		if (name == nullptr)
			continue;
		bool wanted = false;
		for (const char *library : avoided_libraries)
			wanted = wanted || std::strstr(name, library) != nullptr;
		if (!wanted || avoid_count_ == max_ranges)
			continue;
		unsigned long size = 0;
		const auto *header = reinterpret_cast<const mach_header_64 *>(
			_dyld_get_image_header(index));
		const std::uint8_t *const text =
			header == nullptr ? nullptr :
			getsegmentdata(header, "__TEXT", &size);
		if (text == nullptr || size == 0)
			continue;
		avoid_[avoid_count_++] = {reinterpret_cast<std::uintptr_t>(text),
			reinterpret_cast<std::uintptr_t>(text) + size};
	}
}

ThreadFence::~ThreadFence()
{
	release();
}

bool ThreadFence::avoided(std::uintptr_t address) const noexcept
{
	address &= address_mask;
	for (std::size_t index = 0; index < avoid_count_; ++index)
		if (address >= avoid_[index].low && address < avoid_[index].high)
			return true;
	return false;
}

ThreadFence::Held *ThreadFence::find(thread_act_t thread) noexcept
{
	for (std::size_t index = 0; index < count_; ++index)
		if (held_[index].thread == thread)
			return &held_[index];
	return nullptr;
}

/* Whether the stopped thread is in, or called from, a library whose lock it
 * may hold. Frame pointers are kept on Apple's arm64, so the chain of return
 * addresses names every function the thread is inside. */
bool ThreadFence::unsafe_to_hold(thread_act_t thread) const noexcept
{
	arm_thread_state64_t state{};
	mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state(thread, ARM_THREAD_STATE64,
		    reinterpret_cast<thread_state_t>(&state), &words) != KERN_SUCCESS)
		return true;
	if (avoided(static_cast<std::uintptr_t>(arm_thread_state64_get_pc(state))) ||
	    avoided(static_cast<std::uintptr_t>(arm_thread_state64_get_lr(state))))
		return true;
	auto frame_pointer = static_cast<std::uintptr_t>(
		arm_thread_state64_get_fp(state));
	for (int depth = 0; depth < 96 && frame_pointer != 0 &&
	     (frame_pointer & 0xf) == 0; ++depth) {
		std::uintptr_t frame[2] = {};
		vm_size_t read = 0;
		if (vm_read_overwrite(mach_task_self(), frame_pointer, sizeof frame,
			    reinterpret_cast<vm_address_t>(frame), &read) !=
			    KERN_SUCCESS || read != sizeof frame)
			break;
		if (avoided(frame[1]))
			return true;
		if (frame[0] <= frame_pointer)
			break;
		frame_pointer = frame[0];
	}
	return false;
}

bool ThreadFence::try_hold(thread_act_t thread, bool owned, bool named,
	bool registered, Pass &pass) noexcept
{
	for (std::size_t index = 0; index < registry_.creator_count; ++index) {
		if (registry_.creators[index] == thread) {
			++pass.deferred;
			return false;
		}
	}
	const kern_return_t suspended = thread_suspend(thread);
	if (suspended != KERN_SUCCESS)
		/* Ended, or a stale name: there is nothing to hold. */
		return false;
	if (unsafe_to_hold(thread)) {
		(void)thread_resume(thread);
		++pass.deferred;
		++report_.deferred;
		return false;
	}
	if (count_ == capacity) {
		(void)thread_resume(thread);
		report_.failure = too_many_threads;
		return false;
	}
	held_[count_++] = {thread, owned, named, registered};
	++pass.acquired;
	return true;
}

bool ThreadFence::one_pass(thread_act_t self, Pass &pass) noexcept
{
	snapshot_kernel_threads(registry_);
	if (registry_.overflowed) {
		report_.failure = registry_overflowed;
		return false;
	}
	/* A creator whose child is not listed yet may have started it already: it
	 * is let go if it was held, and the pass is not final until the child has
	 * published itself. */
	for (std::size_t index = 0; index < registry_.creator_count; ++index) {
		++pass.deferred;
		if (Held *const creator = find(registry_.creators[index]))
			unhold(creator);
	}
	for (std::size_t index = 0; index < registry_.thread_count; ++index) {
		const thread_act_t thread = registry_.threads[index];
		if (thread == self)
			continue;
		if (Held *const known = find(thread)) {
			known->registered = true;
			continue;
		}
		(void)try_hold(thread, false, false, true, pass);
		if (report_.failure != 0)
			return false;
	}
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		report_.failure = cannot_enumerate;
		return false;
	}
	for (mach_msg_type_number_t index = 0; index < count; ++index) {
		const thread_act_t thread = threads[index];
		bool kept = false;
		if (thread != self) {
			thread_extended_info_data_t information{};
			mach_msg_type_number_t words = THREAD_EXTENDED_INFO_COUNT;
			const bool named = thread_info(thread, THREAD_EXTENDED_INFO,
				reinterpret_cast<thread_info_t>(&information), &words) ==
				KERN_SUCCESS &&
				std::strncmp(information.pth_name, "uml-", 4) == 0;
			if (Held *const known = find(thread)) {
				known->named = known->named || named;
			} else if (named) {
				kept = try_hold(thread, true, true, false, pass);
			}
		}
		if (!kept)
			(void)mach_port_deallocate(mach_task_self(), thread);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	return report_.failure == 0;
}

bool ThreadFence::hold(unsigned budget_ms, Settled settled,
	void *context) noexcept
{
	release_all();
	report_ = {};
	const unsigned long long started = now_ns();
	const thread_act_t self = mach_thread_self();
	set_kernel_thread_reclaim_inhibited(true);
	bool accepted = false;
	unsigned long long creators_since = 0;
	for (;;) {
		++report_.passes;
		Pass pass{};
		if (!one_pass(self, pass))
			break;
		/* A pass that finds everything already held, with no creator still
		 * waiting on a child, is the fixed point. */
		if (pass.acquired == 0 && pass.deferred == 0) {
			if (settled == nullptr || settled(context)) {
				accepted = true;
				break;
			}
			/* The threads must run for the condition to clear. */
			++report_.unsettled;
			release_all();
		}
		/* A creator that is still unfinished after the patience has run out
		 * is stuck behind something a held thread owns. Everything runs
		 * again, which lets it finish, and the hold starts over. */
		bool stalled = false;
		if (registry_.creator_count == 0) {
			creators_since = 0;
		} else if (creators_since == 0) {
			creators_since = now_ns();
		} else if ((now_ns() - creators_since) / nanoseconds_per_millisecond >=
			   creator_patience_ms) {
			++report_.creator_stalls;
			release_all();
			creators_since = 0;
			stalled = true;
		}
		if ((now_ns() - started) / nanoseconds_per_millisecond >= budget_ms) {
			report_.failure = out_of_time;
			break;
		}
		if (stalled)
			pause_for_creators();
		else if (pass.deferred != 0 || pass.acquired == 0)
			pause_briefly();
	}
	(void)mach_port_deallocate(mach_task_self(), self);
	if (!accepted) {
		release();
		return false;
	}
	report_.held = count_;
	for (std::size_t index = 0; index < count_; ++index) {
		report_.named += held_[index].named ? 1 : 0;
		report_.registered += held_[index].registered ? 1 : 0;
	}
	return true;
}

void ThreadFence::unhold(Held *held) noexcept
{
	(void)thread_resume(held->thread);
	if (held->owned)
		(void)mach_port_deallocate(mach_task_self(), held->thread);
	*held = held_[--count_];
}

void ThreadFence::release_all() noexcept
{
	for (std::size_t index = count_; index != 0; --index) {
		const Held &held = held_[index - 1];
		(void)thread_resume(held.thread);
		if (held.owned)
			(void)mach_port_deallocate(mach_task_self(), held.thread);
	}
	count_ = 0;
}

void ThreadFence::release() noexcept
{
	release_all();
	set_kernel_thread_reclaim_inhibited(false);
}

} // namespace uml::runtime
