// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <mach/mach.h>
#include <pthread.h>

namespace uml::runtime {

/*
 * The threads an embedded kernel creates.
 *
 * Anything that must stop every thread able to touch guest RAM needs a list of
 * them. The kernel names some of its threads (`uml-*`) and others carry no
 * name, and a name is set by the thread itself after it has started, so the
 * host cannot find the kernel's threads that way. The loader therefore
 * resolves the kernel's `pthread_create` import to `kernel_pthread_create`,
 * which records each thread it starts before the thread runs a line of kernel
 * code.
 *
 * The registry is a fixed table of atomics: reading it takes no lock and
 * allocates nothing, so it can be read while other threads are stopped,
 * wherever they were stopped. A slot is claimed before `pthread_create` and
 * published by the new thread itself as the first thing it does, so a thread
 * the kernel has started but that has not yet published is visible as a slot
 * whose creator is still in the call.
 */

/* The kernel's `pthread_create`. Signature and result as the system's. */
[[nodiscard]] int kernel_pthread_create(pthread_t *thread,
	const pthread_attr_t *attributes, void *(*start)(void *),
	void *argument);

inline constexpr std::size_t kernel_thread_capacity = 256;

struct KernelThreadSnapshot final {
	/* Thread ports of threads that have published themselves. A thread that
	 * has since ended stays listed until its slot is reused; stopping it
	 * reports KERN_TERMINATED. */
	mach_port_t threads[kernel_thread_capacity];
	std::size_t thread_count;
	/* Threads that are inside `kernel_pthread_create` and whose new thread
	 * has not published itself yet. A creator must not be stopped until its
	 * child is listed, or the child could be started and missed. */
	mach_port_t creators[kernel_thread_capacity];
	std::size_t creator_count;
	/* A thread was started without being recorded because the table was
	 * full. The list is then incomplete for the rest of the process. */
	bool overflowed;
};

/* Takes the registry's state without a lock. */
void snapshot_kernel_threads(KernelThreadSnapshot &snapshot) noexcept;

/* Gives back the slots, and the port rights, of the kernel threads that have
 * ended, and returns how many slots are still in use: threads that run, and
 * creators inside kernel_pthread_create. A guest that restarts itself hands
 * its process back with none left. A thread that was just joined can still be
 * terminating, so the call waits up to 2 s for the slots to empty before it
 * reports them; a thread that runs makes it wait the whole time. Nothing is
 * given back while a fence reads the ports (the next call after it clears
 * does), so a fence in progress shows at once as slots in use. */
[[nodiscard]] std::size_t reclaim_ended_kernel_threads() noexcept;

/* While set, the slots of ended threads are not reclaimed (their ports are
 * not given back), because whoever stopped threads from a snapshot may still
 * be using the ports. A creator that finds the table full waits for the flag
 * to clear. Setting it returns once any reclaim already under way is done. */
void set_kernel_thread_reclaim_inhibited(bool inhibited) noexcept;

} // namespace uml::runtime
