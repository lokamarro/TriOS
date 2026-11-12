// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The panic path's rewrite of kernel frames (runtime/kernel_crash_frames):
 * the address translation, the frame-chain walk over a synthetic chain and over
 * chains a corrupt stack could leave, and the rewrite of another thread's
 * registers and stack, which suspends it for good and so runs in a child
 * process.
 */
#include "kernel_crash_frames.hpp"
#include "require.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::KernelTextAlias;
using uml::runtime::kernel_text_carrier_address;
using uml::runtime::translate_frame_chain;
using uml::runtime::translate_kernel_frames;
using uml::testing::require;

/* Two aliases, as a kernel's init text and main text are, and the carrier
 * bytes they read: none of these addresses is ever dereferenced. */
constexpr std::uintptr_t init_alias = 0x600000004000;
constexpr std::uintptr_t init_carrier = 0x104004000;
constexpr std::uintptr_t text_alias = 0x6000001d4000;
constexpr std::uintptr_t text_carrier = 0x1041d4000;
constexpr KernelTextAlias synthetic[] = {
	{init_alias, init_alias + 0x8000, init_carrier},
	{text_alias, text_alias + 0xa78000, text_carrier},
};
constexpr std::size_t synthetic_count = std::size(synthetic);
constexpr std::uintptr_t host_address = 0x104001234;

void test_translation()
{
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			init_alias) == init_carrier);
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			init_alias + 0x7ffc) == init_carrier + 0x7ffc);
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			init_alias + 0x8000) == init_alias + 0x8000,
		"the end of an alias is outside it");
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			text_alias + 0x3520) == text_carrier + 0x3520);
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			text_alias - 1) == text_alias - 1);
	require(kernel_text_carrier_address(synthetic, synthetic_count,
			host_address) == host_address);
	require(kernel_text_carrier_address(synthetic, 0, init_alias) ==
		init_alias);
	require(kernel_text_carrier_address(nullptr, 0, 0) == 0);
}

/* A chain of frame records laid out in an array: each record is the caller's
 * frame pointer and the return address. */
struct Chain {
	alignas(16) std::uintptr_t words[16]{};

	[[nodiscard]] std::uintptr_t frame(std::size_t index) const noexcept
	{
		return reinterpret_cast<std::uintptr_t>(&words[index * 4]);
	}

	void link(std::size_t index, std::uintptr_t next_frame,
		std::uintptr_t return_address) noexcept
	{
		words[index * 4] = next_frame;
		words[index * 4 + 1] = return_address;
	}
};

void test_chain_rewrite()
{
	Chain chain;
	chain.link(0, chain.frame(1), init_alias + 0x1230);
	chain.link(1, chain.frame(2), host_address);
	chain.link(2, chain.frame(3), text_alias + 0x4560);
	chain.link(3, 0, text_alias + 0x7890);
	require(translate_frame_chain(synthetic, synthetic_count,
			chain.frame(0)) == 3);
	require(chain.words[1] == init_carrier + 0x1230);
	require(chain.words[5] == host_address, "a host return address moved");
	require(chain.words[9] == text_carrier + 0x4560);
	require(chain.words[13] == text_carrier + 0x7890,
		"the outermost record is rewritten too");
	require(chain.words[0] == chain.frame(1) && chain.words[8] == chain.frame(3),
		"a frame pointer changed");
	require(translate_frame_chain(synthetic, synthetic_count,
			chain.frame(0)) == 0, "a rewritten chain is rewritten again");

	Chain untouched;
	untouched.link(0, 0, host_address);
	require(translate_frame_chain(synthetic, synthetic_count,
			untouched.frame(0)) == 0);
	require(untouched.words[1] == host_address);
	require(translate_frame_chain(synthetic, 0, chain.frame(0)) == 0);
}

struct Pages {
	std::size_t size = static_cast<std::size_t>(getpagesize());
	void *base;

	Pages() : base(mmap(nullptr, 2 * size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANON, -1, 0))
	{
		require(base != MAP_FAILED);
	}
	~Pages() { munmap(base, 2 * size); }
	Pages(const Pages &) = delete;
	Pages &operator=(const Pages &) = delete;
	[[nodiscard]] std::uintptr_t second() const noexcept
	{
		return reinterpret_cast<std::uintptr_t>(base) + size;
	}
};

void test_bad_chains_end_the_walk_without_a_fault()
{
	Chain chain;
	chain.link(0, chain.frame(1), text_alias + 0x10);
	chain.link(1, chain.frame(2), text_alias + 0x20);
	chain.link(2, 0, text_alias + 0x30);

	require(translate_frame_chain(synthetic, synthetic_count, 0) == 0);
	require(translate_frame_chain(synthetic, synthetic_count, 0x10) == 0,
		"an unmapped frame");
	require(translate_frame_chain(synthetic, synthetic_count,
			chain.frame(0) + 8) == 0,
		"a frame that is not 16-byte aligned");
	require(chain.words[1] == text_alias + 0x10);

	/* A frame pointer that leads off the end of the readable memory. */
	{
		Pages pages;
		require(mprotect(reinterpret_cast<void *>(pages.second()), pages.size,
				PROT_NONE) == 0);
		const std::uintptr_t last = pages.second() - 16;
		auto *record = reinterpret_cast<std::uintptr_t *>(last);
		record[0] = pages.second();
		record[1] = text_alias + 0x40;
		require(translate_frame_chain(synthetic, synthetic_count, last) == 1,
			"the record before the unreadable page is still rewritten");
		require(record[1] == text_carrier + 0x40);
	}

	/* A record in memory that reads but does not write is left alone. */
	{
		Pages pages;
		auto *record = static_cast<std::uintptr_t *>(pages.base);
		record[0] = 0;
		record[1] = text_alias + 0x50;
		require(mprotect(pages.base, pages.size, PROT_READ) == 0);
		require(translate_frame_chain(synthetic, synthetic_count,
				reinterpret_cast<std::uintptr_t>(record)) == 0);
		require(record[1] == text_alias + 0x50);
	}

	/* A frame that points at itself, and two that point at each other. */
	{
		Chain loop;
		loop.link(0, loop.frame(0), text_alias + 0x60);
		require(translate_frame_chain(synthetic, synthetic_count,
				loop.frame(0)) == 1);
		Chain pair;
		pair.link(0, pair.frame(1), text_alias + 0x70);
		pair.link(1, pair.frame(0), text_alias + 0x80);
		require(translate_frame_chain(synthetic, synthetic_count,
				pair.frame(0)) == 2);
		require(pair.words[1] == text_carrier + 0x70 &&
			pair.words[5] == text_carrier + 0x80);
	}

	/* A chain far longer than any stack ends at the walk's bound. */
	{
		std::vector<std::uintptr_t> deep(4 * 4096 + 2);
		auto base = reinterpret_cast<std::uintptr_t>(deep.data());
		base = (base + 15U) & ~std::uintptr_t{15};
		auto *words = reinterpret_cast<std::uintptr_t *>(base);
		constexpr std::size_t records = 4096;
		for (std::size_t index = 0; index < records; ++index) {
			words[index * 2] = index + 1 < records ?
				base + (index + 1) * 16 : 0;
			words[index * 2 + 1] = text_alias + 0x90;
		}
		const std::size_t rewritten = translate_frame_chain(synthetic,
			synthetic_count, base);
		require(rewritten > 0 && rewritten < records,
			"the walk is bounded and still rewrites what it reached");
	}
}

/* ---- Another thread ----------------------------------------------------- */

/* The functions marked KERNEL_TEXT stand in for kernel code: they share a
 * section of their own, and the alias the test gives the walk covers exactly
 * that section, so the return addresses and program counter of a thread that
 * runs them are the ones the panic path rewrites. */
#define KERNEL_TEXT __attribute__((noinline, section("__TEXT,__kalias,regular,pure_instructions")))

extern "C" const unsigned char kernel_text_begin[]
	__asm("section$start$__TEXT$__kalias");
extern "C" const unsigned char kernel_text_end[]
	__asm("section$end$__TEXT$__kalias");

std::atomic<bool> helper_inside;
std::atomic<std::uintptr_t> helper_frame;
std::atomic<std::uint64_t> helper_progress;

KERNEL_TEXT void alias_touch()
{
	helper_progress.fetch_add(1, std::memory_order_relaxed);
}

KERNEL_TEXT void alias_spin()
{
	helper_frame.store(
		reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)),
		std::memory_order_release);
	helper_inside.store(true, std::memory_order_release);
	for (;;)
		alias_touch();
}

KERNEL_TEXT void alias_park_inner()
{
	alias_spin();
	asm volatile("" ::: "memory");
}

KERNEL_TEXT void alias_park_outer()
{
	alias_park_inner();
	asm volatile("" ::: "memory");
}

KERNEL_TEXT void *alias_thread(void *)
{
	alias_park_outer();
	return nullptr;
}

void check_in_child(const KernelTextAlias &alias);

KERNEL_TEXT void child_caller_b(const KernelTextAlias *alias)
{
	check_in_child(*alias);
	asm volatile("" ::: "memory");
}

KERNEL_TEXT void child_caller_a(const KernelTextAlias *alias)
{
	child_caller_b(alias);
	asm volatile("" ::: "memory");
}

constexpr std::uintptr_t thread_carrier = 0x7000'0000'0000;

/* The frame records of the thread that published `frame`, outermost last. */
std::vector<std::uintptr_t> return_addresses(std::uintptr_t frame)
{
	std::vector<std::uintptr_t> addresses;
	while (frame != 0 && (frame & 15U) == 0 && addresses.size() < 64) {
		const auto *record = reinterpret_cast<const std::uintptr_t *>(frame);
		addresses.push_back(record[1]);
		frame = record[0];
	}
	return addresses;
}

[[nodiscard]] arm_thread_state64_t thread_registers(thread_act_t thread)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
	require(thread_get_state(thread, ARM_THREAD_STATE64,
			reinterpret_cast<thread_state_t>(&state), &words) ==
		KERN_SUCCESS);
	return state;
}

[[nodiscard]] integer_t suspend_count(thread_act_t thread)
{
	thread_basic_info_data_t information;
	mach_msg_type_number_t words = THREAD_BASIC_INFO_COUNT;
	require(thread_info(thread, THREAD_BASIC_INFO,
			reinterpret_cast<thread_info_t>(&information), &words) ==
		KERN_SUCCESS);
	return information.suspend_count;
}

/* Runs in the child outside the alias, below two frames inside it. The rewrite
 * changes its own return address, so it must never return. */
__attribute__((noinline)) void check_in_child(const KernelTextAlias &alias)
{
	pthread_t helper;
	require(pthread_create(&helper, nullptr, &alias_thread, nullptr) == 0);
	while (!helper_inside.load(std::memory_order_acquire))
		usleep(1000);
	usleep(20000);
	const thread_act_t port = pthread_mach_thread_np(helper);
	const std::uintptr_t frame =
		helper_frame.load(std::memory_order_acquire);
	const std::vector<std::uintptr_t> before = return_addresses(frame);
	require(before.size() >= 3);

	const std::uintptr_t own_frame =
		reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
	const std::uintptr_t own_return =
		reinterpret_cast<const std::uintptr_t *>(own_frame)[1];
	require(kernel_text_carrier_address(&alias, 1, own_return) != own_return,
		"the child's own caller is in the alias");

	translate_kernel_frames(&alias, 1);

	require(suspend_count(port) == 1, "the other thread stays suspended");
	const std::vector<std::uintptr_t> after = return_addresses(frame);
	require(after.size() == before.size());
	std::size_t moved = 0;
	for (std::size_t index = 0; index < before.size(); ++index) {
		require(after[index] ==
			kernel_text_carrier_address(&alias, 1, before[index]));
		if (after[index] != before[index])
			++moved;
	}
	require(moved >= 3, "the helper's return addresses into the alias move");

	const arm_thread_state64_t state = thread_registers(port);
	const std::uintptr_t pc = arm_thread_state64_get_pc(state);
	require(pc >= alias.carrier &&
			pc < alias.carrier + (alias.end - alias.begin),
		"the other thread's program counter names the carrier");
	require(arm_thread_state64_get_fp(state) != 0);

	require(reinterpret_cast<const std::uintptr_t *>(own_frame)[1] ==
			kernel_text_carrier_address(&alias, 1, own_return),
		"the calling thread's own chain is rewritten");
	_exit(0);
}

void test_another_threads_registers_and_stack_in_a_child()
{
	const KernelTextAlias alias{
		reinterpret_cast<std::uintptr_t>(kernel_text_begin),
		reinterpret_cast<std::uintptr_t>(kernel_text_end), thread_carrier};
	require(alias.end > alias.begin);
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		child_caller_a(&alias);
		_exit(99);
	}
	int status = 0;
	require(waitpid(child, &status, 0) == child);
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"the child checked another thread's rewritten frames");
}

/* ---- No alias ------------------------------------------------------------ */

std::atomic<std::uint64_t> bystander_progress;

void *bystander(void *)
{
	for (;;) {
		bystander_progress.fetch_add(1, std::memory_order_relaxed);
		usleep(100);
	}
	return nullptr;
}

void test_without_an_alias_no_thread_is_suspended()
{
	pthread_t thread;
	require(pthread_create(&thread, nullptr, &bystander, nullptr) == 0);
	usleep(5000);
	translate_kernel_frames(nullptr, 0);
	translate_kernel_frames(synthetic, 0);
	const std::uint64_t mark = bystander_progress.load();
	usleep(50000);
	require(bystander_progress.load() > mark,
		"with no alias recorded the panic path leaves threads running");
	require(suspend_count(pthread_mach_thread_np(thread)) == 0);
}

/* ---- The record ----------------------------------------------------------- */

void test_the_loaders_record()
{
	using uml::runtime::kernel_text_alias_limit;
	using uml::runtime::record_kernel_text_alias;
	using uml::runtime::recorded_kernel_text_aliases;
	KernelTextAlias copy[kernel_text_alias_limit];
	require(recorded_kernel_text_aliases(copy, kernel_text_alias_limit) == 0);
	const auto *first = reinterpret_cast<const void *>(init_alias);
	const auto *carrier = reinterpret_cast<const void *>(init_carrier);
	require(!record_kernel_text_alias(first, 0, carrier), "an empty range");
	require(!record_kernel_text_alias(first, 0x4000, nullptr), "no carrier");
	require(!record_kernel_text_alias(reinterpret_cast<const void *>(~0ULL),
			0x4000, carrier), "a range that wraps");
	require(record_kernel_text_alias(first, 0x8000, carrier));
	require(record_kernel_text_alias(first, 0x8000, carrier),
		"recording the same range again is accepted");
	require(recorded_kernel_text_aliases(copy, kernel_text_alias_limit) == 1,
		"and changes nothing");
	require(copy[0].begin == init_alias && copy[0].end == init_alias + 0x8000 &&
		copy[0].carrier == init_carrier);
	KernelTextAlias small[1];
	require(recorded_kernel_text_aliases(small, 0) == 1 &&
		recorded_kernel_text_aliases(small, 1) == 1 &&
		small[0].begin == init_alias);
	for (std::size_t index = 1; index < kernel_text_alias_limit; ++index)
		require(record_kernel_text_alias(
			reinterpret_cast<const void *>(text_alias + index * 0x1000000),
			0x4000, carrier));
	require(!record_kernel_text_alias(
			reinterpret_cast<const void *>(text_alias), 0x4000, carrier),
		"a full table refuses another range");
	require(recorded_kernel_text_aliases(copy, kernel_text_alias_limit) ==
		kernel_text_alias_limit);
}

} // namespace

int main()
{
	try {
		test_translation();
		test_chain_rewrite();
		test_bad_chains_end_the_walk_without_a_fault();
		/* The child forks before this process has a thread of its own. */
		test_another_threads_registers_and_stack_in_a_child();
		test_without_an_alias_no_thread_is_suspended();
		test_the_loaders_record();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "kernel crash frames test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("kernel crash frames tests passed");
	return EXIT_SUCCESS;
}
