// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_threads.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <time.h>

namespace uml::runtime {
namespace {

/*
 * A slot is one word, the creator's port in the high half and the new
 * thread's in the low half, so a reader sees a consistent pair:
 *
 *   0                    free
 *   creator, 0           a creator is in kernel_pthread_create and the new
 *                        thread has not published itself
 *   0, thread            the thread is running (or has ended: stopping it
 *                        then reports KERN_TERMINATED)
 *   reclaiming, 0        being freed; nobody may use it
 *
 * The slot's owner is the only writer after the claim, so publication is a
 * plain store.
 */
struct Slot final {
	std::atomic<std::uint64_t> word{0};
	void *(*start)(void *){};
	void *argument{};
};

constexpr std::uint32_t reclaiming = ~std::uint32_t{0};

[[nodiscard]] constexpr std::uint64_t pack(std::uint32_t creator,
	std::uint32_t thread) noexcept
{
	return static_cast<std::uint64_t>(creator) << 32 | thread;
}

std::array<Slot, kernel_thread_capacity> slots;
std::atomic<bool> overflow{false};
std::atomic<bool> reclaim_inhibited{false};
std::atomic<unsigned> reclaims_active{0};

/* A creator that finds the table full while it is inhibited waits for the
 * inhibition to end instead of giving up the list, up to this long. */
constexpr unsigned inhibited_wait_microseconds = 5U * 1000U * 1000U;
constexpr unsigned inhibited_poll_microseconds = 200;

/* A thread whose pthread_join has returned can still be terminating: Darwin
 * wakes the joiner before the thread is gone, so for a moment its port still
 * answers. A request to reclaim waits this long for such threads, and for a
 * creator on its way out of kernel_pthread_create, before it reports a slot
 * in use. */
constexpr unsigned ending_wait_microseconds = 2U * 1000U * 1000U;
constexpr unsigned ending_poll_microseconds = 200;

void pause_for(unsigned microseconds) noexcept
{
	const timespec delay{0, static_cast<long>(microseconds) * 1000L};
	(void)nanosleep(&delay, nullptr);
}

/* Frees the slots of threads that have ended. It is the only place a recorded
 * port is given back, and it never runs while a fence is reading ports. */
void reclaim() noexcept
{
	reclaims_active.fetch_add(1, std::memory_order_seq_cst);
	if (reclaim_inhibited.load(std::memory_order_seq_cst)) {
		reclaims_active.fetch_sub(1, std::memory_order_seq_cst);
		return;
	}
	for (Slot &slot : slots) {
		std::uint64_t seen = slot.word.load(std::memory_order_acquire);
		const auto port = static_cast<mach_port_t>(seen & 0xffffffffU);
		if (port == MACH_PORT_NULL || (seen >> 32) != 0)
			continue;
		thread_basic_info_data_t information{};
		mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
		if (thread_info(port, THREAD_BASIC_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &count) == KERN_SUCCESS)
			continue;
		if (!slot.word.compare_exchange_strong(seen, pack(reclaiming, 0),
			    std::memory_order_acq_rel))
			continue;
		(void)mach_port_deallocate(mach_task_self(), port);
		slot.word.store(0, std::memory_order_release);
	}
	reclaims_active.fetch_sub(1, std::memory_order_seq_cst);
}

[[nodiscard]] Slot *take(mach_port_t creator) noexcept
{
	for (Slot &slot : slots) {
		std::uint64_t expected = 0;
		if (slot.word.compare_exchange_strong(expected, pack(creator, 0),
			    std::memory_order_acq_rel))
			return &slot;
	}
	return nullptr;
}

[[nodiscard]] Slot *claim(mach_port_t creator) noexcept
{
	for (unsigned waited = 0;;) {
		if (Slot *const slot = take(creator))
			return slot;
		if (!reclaim_inhibited.load(std::memory_order_seq_cst)) {
			reclaim();
			return take(creator);
		}
		/* A fence is reading the ports. This thread is one it holds, or is
		 * about to, and goes on when it is let go. */
		if (waited >= inhibited_wait_microseconds)
			return nullptr;
		pause_for(inhibited_poll_microseconds);
		waited += inhibited_poll_microseconds;
	}
}

/* The new thread's first act: it publishes itself, then becomes the kernel's
 * routine. Nothing it does before that touches guest memory. */
void *entry(void *argument) noexcept
{
	auto *slot = static_cast<Slot *>(argument);
	void *(*const start)(void *) = slot->start;
	void *const routine_argument = slot->argument;
	slot->word.store(pack(0, mach_thread_self()), std::memory_order_release);
	return start(routine_argument);
}

} // namespace

int kernel_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
	void *(*start)(void *), void *argument)
{
	Slot *const slot = claim(pthread_mach_thread_np(pthread_self()));
	if (slot == nullptr) {
		overflow.store(true, std::memory_order_release);
		return pthread_create(thread, attributes, start, argument);
	}
	slot->start = start;
	slot->argument = argument;
	const int error = pthread_create(thread, attributes, &entry, slot);
	if (error != 0)
		slot->word.store(0, std::memory_order_release);
	return error;
}

void snapshot_kernel_threads(KernelThreadSnapshot &snapshot) noexcept
{
	snapshot.thread_count = 0;
	snapshot.creator_count = 0;
	snapshot.overflowed = overflow.load(std::memory_order_acquire);
	for (Slot &slot : slots) {
		const std::uint64_t seen = slot.word.load(std::memory_order_acquire);
		const auto creator = static_cast<mach_port_t>(seen >> 32);
		const auto thread = static_cast<mach_port_t>(seen & 0xffffffffU);
		if (thread != MACH_PORT_NULL)
			snapshot.threads[snapshot.thread_count++] = thread;
		else if (creator != MACH_PORT_NULL && creator != reclaiming)
			snapshot.creators[snapshot.creator_count++] = creator;
	}
}

std::size_t reclaim_ended_kernel_threads() noexcept
{
	std::size_t in_use = 0;
	for (unsigned waited = 0;; waited += ending_poll_microseconds) {
		reclaim();
		in_use = 0;
		for (const Slot &slot : slots)
			if (slot.word.load(std::memory_order_acquire) != 0)
				++in_use;
		/* Nothing changes while a fence holds the ports. */
		if (in_use == 0 || waited >= ending_wait_microseconds ||
		    reclaim_inhibited.load(std::memory_order_seq_cst))
			return in_use;
		pause_for(ending_poll_microseconds);
	}
}

void set_kernel_thread_reclaim_inhibited(bool inhibited) noexcept
{
	reclaim_inhibited.store(inhibited, std::memory_order_seq_cst);
	if (!inhibited)
		return;
	/* A reclaim that began before the flag was seen finishes first. */
	while (reclaims_active.load(std::memory_order_seq_cst) != 0)
		pause_for(50);
}

} // namespace uml::runtime
