// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The registry of the threads an embedded kernel creates holds a fixed number
 * of slots. A guest that restarts itself starts a kernel's worth of threads at
 * every boot and joins them at its end, so the registry must give their slots,
 * and the Mach send rights recorded with them, back: a storm of restarts must
 * neither overflow it, which would blind the fence that holds the threads for
 * a hibernation, nor grow the task's port names.
 */
#include "kernel_threads.hpp"
#include "require.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <pthread.h>
#include <time.h>
#include <vector>

namespace {

using uml::runtime::kernel_thread_capacity;
using uml::runtime::KernelThreadSnapshot;
using uml::testing::require;

/* A boot of a kernel with the threads of the Android configuration: ten
 * vCPU carriers, the RTC alarm and the IRQ notifier. */
constexpr unsigned boot_threads = 12;

void *return_at_once(void *) noexcept
{
	return nullptr;
}

void *end_after_a_while(void *) noexcept
{
	const timespec delay{0, 40L * 1000L * 1000L};
	(void)nanosleep(&delay, nullptr);
	return nullptr;
}

std::atomic<bool> release_live_thread{false};

void *wait_until_released(void *) noexcept
{
	while (!release_live_thread.load(std::memory_order_acquire))
		sched_yield();
	return nullptr;
}

[[nodiscard]] KernelThreadSnapshot snapshot()
{
	KernelThreadSnapshot taken{};
	uml::runtime::snapshot_kernel_threads(taken);
	return taken;
}

[[nodiscard]] std::size_t port_names()
{
	mach_port_name_array_t names = nullptr;
	mach_msg_type_number_t name_count = 0;
	mach_port_type_array_t types = nullptr;
	mach_msg_type_number_t type_count = 0;
	require(mach_port_names(mach_task_self(), &names, &name_count, &types,
		&type_count) == KERN_SUCCESS);
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(names), name_count * sizeof(*names));
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(types), type_count * sizeof(*types));
	return name_count;
}

/* One boot: the threads start and the kernel joins them all. */
void boot_and_join(void *(*routine)(void *) = &return_at_once)
{
	std::vector<pthread_t> threads(boot_threads);
	for (auto &thread : threads)
		require(uml::runtime::kernel_pthread_create(&thread, nullptr,
			routine, nullptr) == 0);
	for (const auto thread : threads)
		require(pthread_join(thread, nullptr) == 0);
}

/* Each hand-back leaves no slot in use, however many boots came before. */
void a_restart_storm_cannot_overflow_the_registry()
{
	constexpr unsigned boots = 3 * kernel_thread_capacity / boot_threads;
	static_assert(boots * boot_threads > 2 * kernel_thread_capacity);
	boot_and_join();
	require(uml::runtime::reclaim_ended_kernel_threads() == 0,
		"a joined boot left slots in use");
	const std::size_t ports_before = port_names();
	for (unsigned boot = 0; boot < boots; ++boot) {
		boot_and_join();
		require(!snapshot().overflowed, "the registry overflowed");
		require(uml::runtime::reclaim_ended_kernel_threads() == 0,
			"a hand-back left kernel-thread slots in use");
		const auto taken = snapshot();
		require(taken.thread_count == 0 && taken.creator_count == 0,
			"a snapshot after a hand-back lists threads");
	}
	/* The rights recorded with the slots are given back with them. */
	require(port_names() <= ports_before + boot_threads,
		"the thread ports of ended kernel threads accumulated");
}

/* Without a hand-back, the registry reclaims when it is full, so a launcher
 * that never hands back (power-off) cannot overflow it either. */
void a_registry_without_hand_backs_does_not_overflow()
{
	for (unsigned boot = 0; boot < 3 * kernel_thread_capacity / boot_threads;
	     ++boot) {
		boot_and_join();
		require(!snapshot().overflowed,
			"the registry overflowed without a hand-back");
	}
	require(uml::runtime::reclaim_ended_kernel_threads() == 0);
}

/* A kernel thread that still runs is reported, not reclaimed. */
void a_running_thread_keeps_its_slot()
{
	pthread_t live;
	release_live_thread.store(false, std::memory_order_release);
	require(uml::runtime::kernel_pthread_create(&live, nullptr,
		&wait_until_released, nullptr) == 0);
	boot_and_join();
	require(uml::runtime::reclaim_ended_kernel_threads() == 1,
		"a running kernel thread was reclaimed or lost");
	release_live_thread.store(true, std::memory_order_release);
	require(pthread_join(live, nullptr) == 0);
	require(uml::runtime::reclaim_ended_kernel_threads() == 0,
		"the slot of an ended thread stayed in use");
}

/* A thread that is ending when the hand-back asks, as one just joined is
 * (Darwin wakes the joiner before the thread is gone), is waited for. */
void a_thread_that_is_ending_is_waited_for()
{
	pthread_t ending;
	require(uml::runtime::kernel_pthread_create(&ending, nullptr,
		&end_after_a_while, nullptr) == 0);
	require(uml::runtime::reclaim_ended_kernel_threads() == 0,
		"a thread that was ending was reported as running");
	require(pthread_join(ending, nullptr) == 0);
}

/* While a fence reads the ports nothing is given back, and that shows. */
void nothing_is_reclaimed_while_a_fence_reads_the_ports()
{
	boot_and_join();
	uml::runtime::set_kernel_thread_reclaim_inhibited(true);
	require(uml::runtime::reclaim_ended_kernel_threads() == boot_threads,
		"an inhibited reclaim gave slots back");
	uml::runtime::set_kernel_thread_reclaim_inhibited(false);
	require(uml::runtime::reclaim_ended_kernel_threads() == 0);
}

} // namespace

int main()
{
	a_restart_storm_cannot_overflow_the_registry();
	a_registry_without_hand_backs_does_not_overflow();
	a_running_thread_keeps_its_slot();
	a_thread_that_is_ending_is_waited_for();
	nothing_is_reclaimed_while_a_fence_reads_the_ports();
	std::puts("kernel thread registry tests passed");
	return EXIT_SUCCESS;
}
