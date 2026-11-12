// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_signal_scope.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <stdlib.h>
#include <sys/time.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::atomic_int kernel_deliveries;
std::atomic_int host_deliveries;

using uml::testing::require;

/* Stands in for a handler inside the embedded kernel image. The scopes below
 * are given an image range that covers exactly this function. */
void kernel_handler(int, siginfo_t *, void *)
{
	kernel_deliveries.fetch_add(1, std::memory_order_relaxed);
}

/* A handler the kernel installs from outside its image. */
void foreign_handler(int)
{
}

void host_handler(int)
{
	host_deliveries.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] std::uintptr_t image_begin() noexcept
{
	return reinterpret_cast<std::uintptr_t>(&kernel_handler);
}

[[nodiscard]] std::uintptr_t image_end() noexcept
{
	return image_begin() + 1U;
}

[[nodiscard]] std::uintptr_t handler_of(int signal)
{
	struct sigaction action {};
	require(sigaction(signal, nullptr, &action) == 0,
		"cannot read a signal disposition");
	if ((action.sa_flags & SA_SIGINFO) != 0)
		return reinterpret_cast<std::uintptr_t>(action.sa_sigaction);
	return reinterpret_cast<std::uintptr_t>(action.sa_handler);
}

void install(int signal, void (*handler)(int))
{
	struct sigaction action {};
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	require(sigaction(signal, &action, nullptr) == 0,
		"cannot install a host disposition");
}

/* The flags the Darwin kernel gives its timer handler. */
void install_kernel_handler(int signal)
{
	struct sigaction action {};
	action.sa_sigaction = kernel_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGALRM);
	require(sigaction(signal, &action, nullptr) == 0,
		"cannot install a kernel handler");
}

void change_mask(int how, int signal)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, signal);
	require(pthread_sigmask(how, &set, nullptr) == 0,
		"cannot change the thread signal mask");
}

[[nodiscard]] bool pending_here(int signal)
{
	sigset_t pending;
	sigemptyset(&pending);
	require(sigpending(&pending) == 0, "cannot read pending signals");
	return sigismember(&pending, signal) == 1;
}

[[nodiscard]] sigset_t current_mask()
{
	sigset_t mask;
	require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0,
		"cannot read the thread signal mask");
	return mask;
}

void arm_real_timer()
{
	itimerval timer{};
	timer.it_value.tv_sec = 3600;
	timer.it_interval.tv_sec = 3600;
	require(setitimer(ITIMER_REAL, &timer, nullptr) == 0,
		"cannot arm ITIMER_REAL");
}

[[nodiscard]] bool real_timer_disarmed()
{
	itimerval timer{};
	require(getitimer(ITIMER_REAL, &timer) == 0,
		"cannot read ITIMER_REAL");
	return timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0 &&
		timer.it_interval.tv_sec == 0 && timer.it_interval.tv_usec == 0;
}

void wait_for(const std::atomic_int &phase, int value)
{
	require(uml::testing::waitUntil([&] {
		return phase.load(std::memory_order_acquire) == value;
	}, 10s), "timed out waiting for the other thread");
}

/* Runs on a thread whose SIGALRM mask it inherits from the main thread, and
 * reports the first failure back instead of terminating the process. */
void run_on_thread(const std::function<void()> &body)
{
	std::exception_ptr failure;
	std::thread thread([&] {
		try {
			body();
		} catch (...) {
			failure = std::current_exception();
		}
	});
	thread.join();
	if (failure)
		std::rethrow_exception(failure);
}

void reset_process_state()
{
	for (const int signal : {SIGALRM, SIGUSR1, SIGUSR2, SIGWINCH, SIGPIPE}) {
		install(signal, SIG_IGN);
		install(signal, SIG_DFL);
	}
	const itimerval disarmed{};
	(void)setitimer(ITIMER_REAL, &disarmed, nullptr);
	sigset_t none;
	sigemptyset(&none);
	(void)pthread_sigmask(SIG_SETMASK, &none, nullptr);
	kernel_deliveries.store(0);
	host_deliveries.store(0);
}

/* The hazard the scope exists for: a process-directed signal raised while
 * every thread blocks it stays with the main thread, even after another
 * thread unblocks it, and runs its handler when the main thread unblocks it.
 */
void test_blocked_process_signal_waits_on_main_thread()
{
	reset_process_state();
	install_kernel_handler(SIGALRM);
	change_mask(SIG_BLOCK, SIGALRM);
	run_on_thread([] {
		require(kill(getpid(), SIGALRM) == 0, "cannot raise SIGALRM");
		change_mask(SIG_UNBLOCK, SIGALRM);
		std::this_thread::sleep_for(20ms);
		require(kernel_deliveries.load() == 0,
			"the parked tick moved to a thread which unblocked it");
	});
	require(pending_here(SIGALRM),
		"the blocked tick did not wait on the main thread");
	change_mask(SIG_UNBLOCK, SIGALRM);
	require(kernel_deliveries.load() == 1,
		"unblocking the main thread did not deliver the parked tick");
	reset_process_state();
}

/* The native display case: the kernel runs on a worker while the AppKit
 * thread blocks SIGALRM, and the tick parked on the AppKit thread must not
 * reach the kernel's handler once the image is gone. */
void test_scope_revokes_kernel_signals_from_worker()
{
	reset_process_state();
	install(SIGALRM, host_handler);
	install(SIGUSR1, host_handler);
	change_mask(SIG_BLOCK, SIGALRM);
	std::atomic_int phase{};
	std::exception_ptr worker_failure;
	std::exception_ptr main_failure;
	std::thread worker([&] {
		try {
			change_mask(SIG_UNBLOCK, SIGALRM);
			const sigset_t lent = current_mask();
			{
				const uml::runtime::KernelSignalScope scope{
					image_begin(), image_end()};
				install_kernel_handler(SIGALRM);
				install_kernel_handler(SIGUSR1);
				install_kernel_handler(SIGUSR2);
				install(SIGWINCH, foreign_handler);
				install(SIGPIPE, SIG_IGN);
				change_mask(SIG_BLOCK, SIGALRM);
				change_mask(SIG_BLOCK, SIGPIPE);
				arm_real_timer();
				require(kill(getpid(), SIGALRM) == 0,
					"cannot raise SIGALRM");
				phase.store(1, std::memory_order_release);
				wait_for(phase, 2);
			}
			require(handler_of(SIGALRM) ==
				reinterpret_cast<std::uintptr_t>(&host_handler),
				"SIGALRM did not return to the host handler");
			require(handler_of(SIGUSR1) ==
				reinterpret_cast<std::uintptr_t>(&host_handler),
				"SIGUSR1 did not return to the host handler");
			require(handler_of(SIGUSR2) ==
				reinterpret_cast<std::uintptr_t>(SIG_DFL),
				"SIGUSR2 did not return to its default action");
			require(handler_of(SIGWINCH) ==
				reinterpret_cast<std::uintptr_t>(SIG_DFL),
				"a handler the kernel set outside the image survived");
			require(handler_of(SIGPIPE) ==
				reinterpret_cast<std::uintptr_t>(SIG_DFL),
				"a signal the kernel ignored stayed ignored");
			require(real_timer_disarmed(),
				"ITIMER_REAL survived the kernel");
			const sigset_t returned = current_mask();
			require(returned == lent,
				"the lent thread came back with another mask");
		} catch (...) {
			worker_failure = std::current_exception();
		}
		phase.store(3, std::memory_order_release);
	});
	bool parked = false;
	try {
		wait_for(phase, 1);
		parked = pending_here(SIGALRM);
	} catch (...) {
		main_failure = std::current_exception();
	}
	phase.store(2, std::memory_order_release);
	worker.join();
	if (worker_failure)
		std::rethrow_exception(worker_failure);
	if (main_failure)
		std::rethrow_exception(main_failure);
	require(parked, "the test did not park a tick on the main thread");
	require(!pending_here(SIGALRM),
		"the parked tick survived the revocation");
	change_mask(SIG_UNBLOCK, SIGALRM);
	require(kernel_deliveries.load() == 0,
		"a revoked kernel handler ran");
	require(host_deliveries.load() == 0,
		"a tick raised for the kernel reached the host handler");
	reset_process_state();
}

/* The headless case: the kernel runs on the main thread itself. */
void test_scope_revokes_kernel_signals_on_calling_thread()
{
	reset_process_state();
	const sigset_t lent = current_mask();
	{
		const uml::runtime::KernelSignalScope scope{
			image_begin(), image_end()};
		install_kernel_handler(SIGALRM);
		change_mask(SIG_BLOCK, SIGALRM);
		arm_real_timer();
		require(kill(getpid(), SIGALRM) == 0, "cannot raise SIGALRM");
		require(pending_here(SIGALRM),
			"the test did not park a tick on the calling thread");
	}
	const sigset_t returned = current_mask();
	require(returned == lent,
		"the calling thread came back with another mask");
	require(handler_of(SIGALRM) ==
		reinterpret_cast<std::uintptr_t>(SIG_DFL),
		"SIGALRM did not return to its default action");
	require(real_timer_disarmed(), "ITIMER_REAL survived the kernel");
	require(!pending_here(SIGALRM),
		"the parked tick survived the revocation");
	require(kernel_deliveries.load() == 0, "a revoked kernel handler ran");
	reset_process_state();
}

/* The next boot of a guest that restarted itself finds the dispositions the
 * first one found, wherever the kernel left them: ignored, defaulted, set to a
 * host handler or given other flags or a different mask. A disposition the
 * kernel did not change is not touched. */
void test_scope_restores_every_changed_disposition()
{
	reset_process_state();
	install(SIGUSR1, host_handler);
	install(SIGUSR2, SIG_IGN);
	install(SIGALRM, host_handler);
	struct sigaction flagged {};
	flagged.sa_handler = host_handler;
	sigemptyset(&flagged.sa_mask);
	sigaddset(&flagged.sa_mask, SIGUSR1);
	flagged.sa_flags = SA_RESTART;
	require(sigaction(SIGWINCH, &flagged, nullptr) == 0);
	{
		const uml::runtime::KernelSignalScope scope{
			image_begin(), image_end()};
		install(SIGPIPE, SIG_IGN);
		install(SIGUSR1, SIG_DFL);
		install(SIGUSR2, host_handler);
		/* The same handler with other flags is a change. */
		struct sigaction other = flagged;
		other.sa_flags = SA_RESTART | SA_NODEFER;
		require(sigaction(SIGWINCH, &other, nullptr) == 0);
		/* One the kernel leaves as it found it, in the host's own handler. */
		struct sigaction again {};
		require(sigaction(SIGALRM, nullptr, &again) == 0);
		require(sigaction(SIGALRM, &again, nullptr) == 0);
	}
	require(handler_of(SIGPIPE) == reinterpret_cast<std::uintptr_t>(SIG_DFL),
		"SIGPIPE stayed ignored");
	require(handler_of(SIGUSR1) ==
		reinterpret_cast<std::uintptr_t>(&host_handler),
		"a defaulted host handler was not restored");
	require(handler_of(SIGUSR2) == reinterpret_cast<std::uintptr_t>(SIG_IGN),
		"an ignored signal that the kernel handled was not restored");
	struct sigaction restored {};
	require(sigaction(SIGWINCH, nullptr, &restored) == 0);
	require(restored.sa_handler == host_handler &&
		restored.sa_flags == SA_RESTART &&
		restored.sa_mask == flagged.sa_mask,
		"a disposition with changed flags was not restored whole");
	require(handler_of(SIGALRM) ==
		reinterpret_cast<std::uintptr_t>(&host_handler),
		"a disposition the kernel did not change was disturbed");
	reset_process_state();
}

[[nodiscard]] stack_t current_stack()
{
	stack_t stack{};
	require(sigaltstack(nullptr, &stack) == 0,
		"cannot read the signal stack");
	return stack;
}

[[nodiscard]] bool stack_disabled(const stack_t &stack)
{
	return (stack.ss_flags & SS_DISABLE) != 0;
}

/* Installs `stack`, disabling the stack if it is disabled. */
void install_stack(const stack_t &stack)
{
	stack_t installed = stack;
	if (stack_disabled(stack)) {
		installed.ss_sp = nullptr;
		installed.ss_size = MINSIGSTKSZ;
		installed.ss_flags = SS_DISABLE;
	}
	require(sigaltstack(&installed, nullptr) == 0,
		"cannot install the signal stack");
}

/* The kernel gives the thread that entered it a stack it allocated itself,
 * which the ledger frees afterwards. The thread must have its own again by
 * then, or none if it had none. */
void test_scope_restores_the_calling_threads_signal_stack()
{
	run_on_thread([] {
		/* A sanitizer runtime gives every thread a stack of its own, which
		 * it frees when the thread ends: begin with none, and leave the
		 * thread as it was found. */
		const stack_t found = current_stack();
		stack_t none{};
		none.ss_flags = SS_DISABLE;
		install_stack(none);
		require(stack_disabled(current_stack()),
			"the signal stack could not be disabled");
		std::vector<char> kernel_stack(64U * 1024U);
		{
			const uml::runtime::KernelSignalScope scope{
				image_begin(), image_end()};
			stack_t installed{};
			installed.ss_sp = kernel_stack.data();
			installed.ss_size = kernel_stack.size();
			require(sigaltstack(&installed, nullptr) == 0);
		}
		require(stack_disabled(current_stack()),
			"the kernel's signal stack outlived the scope");

		std::vector<char> host_stack(64U * 1024U);
		stack_t host{};
		host.ss_sp = host_stack.data();
		host.ss_size = host_stack.size();
		require(sigaltstack(&host, nullptr) == 0);
		{
			const uml::runtime::KernelSignalScope scope{
				image_begin(), image_end()};
			stack_t installed{};
			installed.ss_sp = kernel_stack.data();
			installed.ss_size = kernel_stack.size();
			require(sigaltstack(&installed, nullptr) == 0);
		}
		const stack_t after = current_stack();
		require(!stack_disabled(after) && after.ss_sp == host.ss_sp &&
			after.ss_size == host.ss_size,
			"the host's own signal stack was not restored");
		/* A scope that sees no change leaves the stack alone. */
		{
			const uml::runtime::KernelSignalScope scope{
				image_begin(), image_end()};
		}
		const stack_t untouched = current_stack();
		require(untouched.ss_sp == host.ss_sp &&
			untouched.ss_size == host.ss_size);
		install_stack(found);
	});
}

/* Descriptors 0 to 2 of the process, each its own open file description of a
 * terminal the test owns. */
class StandardDescriptors final {
public:
	StandardDescriptors()
	{
		master_ = posix_openpt(O_RDWR | O_NOCTTY);
		require(master_ >= 0 && grantpt(master_) == 0 &&
			unlockpt(master_) == 0, "cannot open a terminal");
		const char *name = ptsname(master_);
		require(name != nullptr);
		for (int descriptor = 0; descriptor < 3; ++descriptor) {
			saved_[descriptor] = fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
			require(saved_[descriptor] >= 0);
			const int slave = open(name, O_RDWR | O_NOCTTY);
			require(slave >= 0, "cannot open the terminal's other side");
			require(dup2(slave, descriptor) == descriptor);
			close(slave);
		}
	}

	~StandardDescriptors()
	{
		restore();
		close(master_);
	}

	/* The test's stderr must work again before it reports. */
	void restore()
	{
		for (int descriptor = 0; descriptor < 3; ++descriptor) {
			if (saved_[descriptor] < 0)
				continue;
			(void)dup2(saved_[descriptor], descriptor);
			close(saved_[descriptor]);
			saved_[descriptor] = -1;
		}
	}

private:
	int master_{-1};
	int saved_[3]{-1, -1, -1};
};

/* The kernel's console channel makes a descriptor non-blocking and its terminal
 * raw. Only the status flags F_SETFL can set are compared, and a descriptor the
 * kernel left alone keeps what it had. */
void test_scope_restores_the_standard_descriptors()
{
	bool nonblocking_gone = false;
	bool append_kept = false;
	bool untouched_stays = false;
	bool terminal_back = false;
	{
		StandardDescriptors standard;
		struct termios cooked {};
		const bool cooked_read = tcgetattr(0, &cooked) == 0;
		(void)fcntl(2, F_SETFL, fcntl(2, F_GETFL) | O_APPEND);
		{
			const uml::runtime::KernelSignalScope scope{
				image_begin(), image_end()};
			(void)fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
			(void)fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
			struct termios raw = cooked;
			cfmakeraw(&raw);
			(void)tcsetattr(0, TCSANOW, &raw);
			/* A write sets the "was written" bit Darwin reports in F_GETFL,
			 * which no call clears. */
			const char byte = '\n';
			(void)!write(1, &byte, 1);
		}
		nonblocking_gone = (fcntl(0, F_GETFL) & O_NONBLOCK) == 0 &&
			(fcntl(1, F_GETFL) & O_NONBLOCK) == 0;
		append_kept = (fcntl(2, F_GETFL) & O_APPEND) != 0;
		untouched_stays = (fcntl(2, F_GETFL) & O_NONBLOCK) == 0;
		struct termios back {};
		terminal_back = cooked_read && tcgetattr(0, &back) == 0 &&
			back.c_iflag == cooked.c_iflag &&
			back.c_oflag == cooked.c_oflag &&
			back.c_cflag == cooked.c_cflag &&
			(back.c_lflag & ~PENDIN) == (cooked.c_lflag & ~PENDIN) &&
			std::memcmp(back.c_cc, cooked.c_cc, sizeof(back.c_cc)) == 0;
		standard.restore();
	}
	require(nonblocking_gone, "O_NONBLOCK survived on a standard descriptor");
	require(append_kept, "a flag the kernel did not change was removed");
	require(untouched_stays,
		"a descriptor the kernel left alone gained a flag");
	require(terminal_back, "the console terminal stayed raw");
}

} // namespace

int main()
{
	try {
		test_blocked_process_signal_waits_on_main_thread();
		test_scope_revokes_kernel_signals_from_worker();
		test_scope_revokes_kernel_signals_on_calling_thread();
		test_scope_restores_every_changed_disposition();
		test_scope_restores_the_calling_threads_signal_stack();
		test_scope_restores_the_standard_descriptors();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "kernel signal scope test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("kernel signal scope tests passed");
	return EXIT_SUCCESS;
}
