// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A host application can call exit() while its guest still runs: an iOS
 * application ends through exit() after it has been asked to terminate, and
 * so does an XCTest host when its tests finish. exit() runs the destructors
 * of objects with static storage duration while the vCPU carriers keep
 * running, so nothing a carrier reaches may have one.
 *
 * The child process keeps carriers resolving scalar and vector gadget
 * contracts, mapping through the shared-backing layer, and entering and
 * leaving the software address space while its main thread calls exit().
 * Handlers registered with atexit() run in the reverse order of registration,
 * interleaved with those destructors. The hold is registered while this
 * file's statics are initialized; engine state is created lazily, or by
 * libraries linked after this file's object, so the hold runs after every
 * destructor exit() would run and keeps the process alive long enough for
 * the carriers to reach what was destroyed.
 *
 * A destroyed mutex fails to lock only when it was free at its destruction,
 * so the carriers that lock one pace themselves to leave it free.
 */
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "memory/domain_contract.hpp"
#include "memory/shared_backing.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace g = uml::cpu::arm64::gadgets;

namespace {

constexpr auto hold = std::chrono::milliseconds{200};
constexpr auto pace = std::chrono::milliseconds{1};
constexpr unsigned translation_carriers = 2;
constexpr std::uint64_t test_domain = 7;
constexpr unsigned long test_generation = 1;

std::atomic<std::uint64_t> translations{};
std::atomic<std::uint64_t> mappings{};
std::atomic<std::uint64_t> accesses{};

/* Registered while this file's statics are initialized; the process is
 * forked afterwards, and the child inherits the registration. */
const bool hold_registered = [] {
	return std::atexit([] { std::this_thread::sleep_for(hold); }) == 0;
}();

bool provide_test_domain(std::uint64_t &domain) noexcept
{
	domain = test_domain;
	return true;
}

std::vector<std::uintptr_t> words_of(const g::ScalarContract &contract)
{
	std::vector<std::uintptr_t> words(1U + contract.operand_words);
	words[0] = contract.entries[0];
	return words;
}

[[noreturn]] void fail_child(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::_Exit(2);
}

[[noreturn]] void run_carriers_through_exit(int backing)
{
	const auto scalar = g::scalar_contracts();
	const auto vector = g::vector_contracts();
	if (scalar.empty() || vector.empty())
		fail_child("no gadget contracts are linked");
	const auto scalar_words = words_of(scalar[0]);
	const auto vector_words = words_of(vector[0]);
	if (g::scalar_access(scalar_words).contract != &scalar[0] ||
	    g::vector_access(vector_words).contract != &vector[0])
		fail_child("a contract does not resolve to itself");
	uml::memory::set_domain_provider(&provide_test_domain);
	if (darwin_uml_static_vm_activate(test_generation) != 0)
		fail_child("the test domain cannot be activated");

	for (unsigned carrier = 0; carrier < translation_carriers; ++carrier)
		std::thread{[=] {
			for (;;) {
				(void)g::scalar_access(scalar_words);
				(void)g::vector_access(vector_words);
				translations.fetch_add(1, std::memory_order_relaxed);
			}
		}}.detach();
	std::thread{[=] {
		const auto page = static_cast<std::size_t>(getpagesize());
		for (;;) {
			void *mapped = uml::memory::map_shared_backing(nullptr, page,
				PROT_READ, MAP_SHARED, backing, 0);
			if (mapped != MAP_FAILED)
				(void)munmap(mapped, page);
			mappings.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::sleep_for(pace);
		}
	}}.detach();
	/* Each thread takes a slot in the active-access registry on its first
	 * access and leaves it when it ends. */
	std::thread{[] {
		for (;;) {
			std::thread{[] {
				(void)darwin_uml_static_vm_activate(test_generation);
				accesses.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::sleep_for(2 * pace);
			}}.detach();
			std::this_thread::sleep_for(pace);
		}
	}}.detach();

	while (translations.load(std::memory_order_relaxed) < 10'000 ||
	       mappings.load(std::memory_order_relaxed) < 20 ||
	       accesses.load(std::memory_order_relaxed) < 20)
		std::this_thread::sleep_for(pace);
	std::exit(0);
}

} // namespace

int main()
{
	if (!hold_registered) {
		std::fputs("cannot register the exit hold\n", stderr);
		return 1;
	}
	const uml::testing::ScratchBacking backing{"gadget-exit-backing-XXXXXX",
		static_cast<std::size_t>(getpagesize())};

	const pid_t child = fork();
	if (child < 0) {
		std::perror("fork");
		return 1;
	}
	if (child == 0)
		run_carriers_through_exit(backing.descriptor());

	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	int status = 0;
	for (;;) {
		const pid_t ended = waitpid(child, &status, WNOHANG);
		if (ended == child)
			break;
		if (ended < 0) {
			std::perror("waitpid");
			return 1;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			std::fputs("the exiting process did not end\n", stderr);
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	if (WIFSIGNALED(status)) {
		std::fprintf(stderr,
			"a carrier ended its process with signal %d while it exited\n",
			WTERMSIG(status));
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		std::fprintf(stderr, "the exiting process returned %d\n",
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return 1;
	}
	std::puts("carriers kept running through exit");
	return 0;
}
