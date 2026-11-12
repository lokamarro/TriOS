// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: unwinding through an arm64 signal frame works as on native
// Linux.
//
// glibc installs handlers without SA_RESTORER, so a handler returns to the
// kernel's sigreturn trampoline. libgcc finds the interrupted frame by reading
// the "mov x8, #139; svc #0" sequence at that return address, which backtrace()
// in a handler and the forced unwind of a thread cancelled while blocked in a
// system call both rely on. The handler's caller frame record must also hold
// the interrupted x29 and x30, as the native frame record above the signal
// frame does. A debugger can also write the page that holds those
// instructions, which becomes the tracee's private copy, as it can native's
// sigreturn page. Every case runs in its own child, so a crash fails only
// that case. Prints one line per case and exits with the number of failures.
//
// Build in the guest: c++ -std=c++20 -O2 -g -fno-omit-frame-pointer -rdynamic
// -pthread signal_unwind_probe.cpp

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// backtrace_symbols() names only exported functions, so the function that
// raises the signal has external linkage and the probe links with -rdynamic.
extern "C" [[gnu::noinline]] void raise_in_signal_origin()
{
	raise(SIGUSR1);
	asm volatile("" ::: "memory");
}

namespace {

constexpr int maximum_frames = 64;

void *handler_frames[maximum_frames];
volatile int handler_depth;
volatile std::uintptr_t record_fp;
volatile std::uintptr_t record_lr;
volatile std::uintptr_t interrupted_fp;
volatile std::uintptr_t interrupted_lr;

void take_backtrace(int)
{
	handler_depth = backtrace(handler_frames, maximum_frames);
}

void read_frame_record(int, siginfo_t *, void *context)
{
	const auto *uc = static_cast<const ucontext_t *>(context);
	// The handler's own frame record links to the one the kernel placed
	// above the signal frame, which holds the interrupted x29 and x30.
	const auto *frame =
		static_cast<const std::uintptr_t *>(__builtin_frame_address(0));
	const auto *record = reinterpret_cast<const std::uintptr_t *>(frame[0]);

	if (record != nullptr) {
		record_fp = record[0];
		record_lr = record[1];
	}
	interrupted_fp = uc->uc_mcontext.regs[29];
	interrupted_lr = uc->uc_mcontext.regs[30];
}

volatile std::uintptr_t handler_return;

[[gnu::noinline]] void read_return_address(int)
{
	handler_return = reinterpret_cast<std::uintptr_t>(
		__builtin_return_address(0));
}

bool frames_name(char **symbols, int count, const char *name)
{
	for (int index = 0; index < count; ++index) {
		if (symbols[index] != nullptr &&
		    std::strstr(symbols[index], name) != nullptr)
			return true;
	}
	return false;
}

// backtrace() in a handler unwinds through the signal frame into the code
// that raised the signal and on to main().
int backtrace_case()
{
	if (signal(SIGUSR1, take_backtrace) == SIG_ERR)
		return 2;
	raise_in_signal_origin();
	const int depth = handler_depth;
	char **symbols = backtrace_symbols(handler_frames, depth);
	if (symbols == nullptr)
		return 3;
	const bool origin = frames_name(symbols, depth, "raise_in_signal_origin");
	const bool main_frame = frames_name(symbols, depth, "main");
	if (!origin || !main_frame) {
		for (int index = 0; index < depth; ++index)
			std::printf("  frame %d: %s\n", index, symbols[index]);
		std::free(symbols);
		return 4;
	}
	std::free(symbols);
	return 0;
}

// The handler's caller frame record holds the interrupted x29 and x30.
int frame_record_case()
{
	struct sigaction action = {};
	action.sa_sigaction = read_frame_record;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, nullptr) != 0)
		return 2;
	raise_in_signal_origin();
	if (record_fp != interrupted_fp || record_lr != interrupted_lr) {
		std::printf("  record fp=%#lx lr=%#lx, interrupted fp=%#lx "
			    "lr=%#lx\n",
			    static_cast<unsigned long>(record_fp),
			    static_cast<unsigned long>(record_lr),
			    static_cast<unsigned long>(interrupted_fp),
			    static_cast<unsigned long>(interrupted_lr));
		return 4;
	}
	return 0;
}

// A tracer reads the word at the handler's return address and writes it
// back, as a debugger setting a breakpoint there does.
int trampoline_poke_case()
{
	if (signal(SIGUSR1, read_return_address) == SIG_ERR)
		return 2;
	raise_in_signal_origin();
	const std::uintptr_t address = handler_return;
	const pid_t tracee = fork();
	if (tracee < 0)
		return 2;
	if (tracee == 0) {
		ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
		raise(SIGSTOP);
		std::_Exit(0);
	}
	int status = 0;
	if (waitpid(tracee, &status, 0) != tracee || !WIFSTOPPED(status))
		return 3;
	auto *target = reinterpret_cast<void *>(address);
	errno = 0;
	const long word = ptrace(PTRACE_PEEKTEXT, tracee, target, nullptr);
	const int peek_error = errno;
	const long poke = peek_error == 0 ?
		ptrace(PTRACE_POKETEXT, tracee, target,
		       reinterpret_cast<void *>(word)) : -1;
	const int poke_error = errno;
	errno = 0;
	const long again = ptrace(PTRACE_PEEKTEXT, tracee, target, nullptr);
	kill(tracee, SIGKILL);
	waitpid(tracee, &status, 0);
	if (peek_error != 0 || poke != 0 || again != word) {
		std::printf("  at %#lx: peek %s, poke %s, word %#lx then %#lx\n",
			    static_cast<unsigned long>(address),
			    std::strerror(peek_error),
			    poke == 0 ? "ok" : std::strerror(poke_error),
			    static_cast<unsigned long>(word),
			    static_cast<unsigned long>(again));
		return 4;
	}
	return 0;
}

int pipe_descriptors[2];
volatile bool cleanup_ran;

void mark_cleanup(void *)
{
	cleanup_ran = true;
}

// The thread a cancellation case is about, once it is about to make its call.
std::atomic<int> worker_tid;

void *blocked_reader(void *)
{
	pthread_cleanup_push(mark_cleanup, nullptr);
	worker_tid.store(static_cast<int>(syscall(SYS_gettid)));
	char byte;
	(void)read(pipe_descriptors[0], &byte, 1);
	pthread_cleanup_pop(0);
	return nullptr;
}

void *polling_worker(void *)
{
	pthread_cleanup_push(mark_cleanup, nullptr);
	worker_tid.store(static_cast<int>(syscall(SYS_gettid)));
	for (;;)
		pthread_testcancel();
	pthread_cleanup_pop(0);
	return nullptr;
}

// The scheduling state of a thread of this process, or '?'.
char task_state(int tid)
{
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
	std::FILE *file = std::fopen(path, "r");
	if (file == nullptr)
		return '?';
	char line[512] = "";
	const bool read_line = std::fgets(line, sizeof(line), file) != nullptr;
	std::fclose(file);
	// The state follows the command name, which is in parentheses.
	const char *name_end = read_line ? std::strrchr(line, ')') : nullptr;
	return name_end != nullptr && name_end[1] == ' ' ? name_end[2] : '?';
}

// Waits, for at most ten seconds, until the worker is in its call: for a
// reader that is asleep in read(), as only then does the cancellation signal
// interrupt a system call. A loaded guest can schedule the worker long after
// a fixed pause.
bool wait_for_worker(bool must_sleep)
{
	for (int attempt = 0; attempt < 10000; ++attempt) {
		const int tid = worker_tid.load();
		if (tid != 0 && (!must_sleep || task_state(tid) == 'S'))
			return true;
		usleep(1000);
	}
	return false;
}

// Cancelling a thread runs its cleanup handler through a forced unwind. For
// a thread blocked in read(), that unwind starts in the cancellation
// signal's handler and must cross its signal frame.
int cancel_case(void *(*worker)(void *))
{
	if (pipe(pipe_descriptors) != 0)
		return 2;
	pthread_t thread;
	if (pthread_create(&thread, nullptr, worker, nullptr) != 0)
		return 2;
	if (!wait_for_worker(worker == blocked_reader)) {
		std::printf("  the worker never reached its call\n");
		return 5;
	}
	pthread_cancel(thread);
	void *result = nullptr;
	pthread_join(thread, &result);
	if (result != PTHREAD_CANCELED || !cleanup_ran) {
		std::printf("  result %s, cleanup %s\n",
			    result == PTHREAD_CANCELED ? "cancelled" : "returned",
			    cleanup_ran ? "ran" : "skipped");
		return 4;
	}
	return 0;
}

struct Case {
	const char *name;
	int (*run)();
};

const Case cases[] = {
	{"deferred cancellation at pthread_testcancel",
	 [] { return cancel_case(polling_worker); }},
	{"backtrace() through a signal frame", backtrace_case},
	{"frame record above a signal frame", frame_record_case},
	{"cancellation of a thread blocked in read()",
	 [] { return cancel_case(blocked_reader); }},
	{"tracer write at the sigreturn trampoline", trampoline_poke_case},
};

} // namespace

int main()
{
	int failures = 0;
	for (const auto &entry : cases) {
		std::fflush(stdout);
		const pid_t child = fork();
		if (child < 0)
			return 100;
		if (child == 0) {
			alarm(30);
			std::_Exit(entry.run());
		}
		int status = 0;
		if (waitpid(child, &status, 0) != child)
			return 100;
		std::string detail;
		if (WIFSIGNALED(status))
			detail = "killed by signal " + std::to_string(WTERMSIG(status));
		else if (WEXITSTATUS(status) != 0)
			detail = "exit status " + std::to_string(WEXITSTATUS(status));
		std::printf("%s %s%s%s\n", detail.empty() ? "PASS" : "FAIL",
			    entry.name, detail.empty() ? "" : ": ", detail.c_str());
		failures += detail.empty() ? 0 : 1;
	}
	return failures;
}
