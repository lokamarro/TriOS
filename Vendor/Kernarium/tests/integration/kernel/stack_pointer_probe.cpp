// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: a bad stack pointer is the task's own business.
//
// A task may hold any value in SP. Pinned to CPU 0, so that every event
// reaches one vCPU, the probe makes system calls and faults a store through
// SP, again and again, with SP at zero and at and above the end of the task's
// address space, 128 KiB aligned and not. As on native arm64, every system
// call returns and every store delivers SIGSEGV with SEGV_MAPERR at SP to a
// handler on the alternate stack; the kernel never kills the task. Every
// stack pointer runs in its own child, so a crash fails only that case.
// Prints one line per case and exits with the number of failures.
//
// Build in the guest: c++ -std=c++20 -O2 stack_pointer_probe.cpp

#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// getpid_on(sp, count) makes count getpid calls with SP = sp and returns the
// last result. store_on(sp) stores through SP = sp. x9 keeps the caller's SP;
// system calls and signal delivery preserve it.
extern "C" std::uint64_t getpid_on(std::uint64_t stack_pointer,
				   std::uint64_t count);
extern "C" void store_on(std::uint64_t stack_pointer);
extern "C" char store_on_end[];

asm(R"(
	.text
	.p2align 2
	.global getpid_on
getpid_on:
	mov x9, sp
	mov sp, x0
1:	mov x8, #172
	svc #0
	subs x1, x1, #1
	b.ne 1b
	mov sp, x9
	ret
	.global store_on
store_on:
	mov x9, sp
	mov sp, x0
	str xzr, [sp]
	.global store_on_end
store_on_end:
	mov sp, x9
	ret
)");

namespace {

constexpr int rounds = 16;

volatile sig_atomic_t faults;
volatile int fault_code;
void *volatile fault_address;
alignas(16) unsigned char alternate_stack[64 * 1024];

void on_fault(int, siginfo_t *info, void *context)
{
	faults = faults + 1;
	fault_code = info->si_code;
	fault_address = info->si_addr;
	static_cast<ucontext_t *>(context)->uc_mcontext.pc =
		reinterpret_cast<std::uint64_t>(store_on_end);
}

bool prepare()
{
	cpu_set_t first{};
	CPU_SET(0, &first);
	stack_t stack{};
	stack.ss_sp = alternate_stack;
	stack.ss_size = sizeof(alternate_stack);
	struct sigaction action{};
	action.sa_sigaction = on_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	if (sched_setaffinity(0, sizeof(first), &first) == 0 &&
	    sigaltstack(&stack, nullptr) == 0 &&
	    sigaction(SIGSEGV, &action, nullptr) == 0)
		return true;
	std::perror("stack-pointer probe setup");
	return false;
}

// Exits 0 when every call returned the pid and every store faulted once at
// SP.
[[noreturn]] void run(std::uint64_t stack_pointer)
{
	if (!prepare())
		std::_Exit(2);
	const auto pid = static_cast<std::uint64_t>(getpid());
	for (int round = 0; round < rounds; ++round) {
		if (getpid_on(stack_pointer, rounds) != pid) {
			std::printf("  getpid returned the wrong pid\n");
			std::_Exit(1);
		}
		faults = 0;
		store_on(stack_pointer);
		if (faults != 1 || fault_code != SEGV_MAPERR ||
		    fault_address != reinterpret_cast<void *>(stack_pointer)) {
			std::printf("  store: %d SIGSEGV, code %d, address %p\n",
				    static_cast<int>(faults), fault_code,
				    fault_address);
			std::_Exit(1);
		}
	}
	std::_Exit(0);
}

} // namespace

int main()
{
	const struct {
		const char *name;
		std::uint64_t value;
	} pointers[] = {
		{"zero", 0},
		{"the end of the address space", 0x0000'8000'0000'0000},
		{"16 bytes past the end", 0x0000'8000'0000'0010},
		{"2^48", 0x0001'0000'0000'0000},
	};
	int failures = 0;
	for (const auto &pointer : pointers) {
		std::fflush(stdout);
		const pid_t child = fork();
		if (child == 0)
			run(pointer.value);
		int status = 0;
		const bool passed = child > 0 &&
			waitpid(child, &status, 0) == child &&
			WIFEXITED(status) && WEXITSTATUS(status) == 0;
		std::printf("%s SP %s: %d calls and %d stores", passed ?
			    "PASS" : "FAIL", pointer.name, rounds * rounds,
			    rounds);
		if (WIFSIGNALED(status))
			std::printf(", killed by signal %d", WTERMSIG(status));
		std::printf("\n");
		failures += passed ? 0 : 1;
	}
	return failures;
}
