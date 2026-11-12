// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Stops embedded guests at varied points of their boot and checks that each
 * run ends cleanly. The loader unmaps the kernel image as soon as the kernel
 * entry returns, so a run end must first see every kernel thread leave it.
 * A process boots one guest, so every attempt is a child process of its own.
 * A child lingers after its run returns, so that a thread still inside the
 * image faults there, and then requires that the run left no thread behind.
 *
 * A vCPU carrier signals the run end from its thread-key destructor and still
 * returns through image code afterwards. That window is a few instructions
 * wide, so a child widens it: once it has asked for a stop, a carrier that
 * broadcasts a condition sleeps after its next mutex unlock. The kernel
 * resolves these imports by name, and this executable exports its own.
 *
 * Like every embedder, a child keeps the kernel's early process-directed
 * SIGALRM blocked on its own threads; only vCPU0 may take it. One more child
 * thread breaks that rule on purpose once the kernel handles SIGALRM: it
 * unblocks the signal and sends it to itself. The kernel must refuse to run a
 * guest tick there and leave SIGALRM blocked on that thread.
 *
 * The run-end and SIGALRM contracts are in kernel/common/README.md.
 */
#include "uml/emulator.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <mach/mach.h>
#include <memory>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/* Stop delays, measured from the kernel's first call into this executable.
 * A stop requested earlier, while the run is still queued at admission,
 * cancels the run instead (uml/emulator.h), and a loaded host can take longer
 * than the shortest delay to get there. The first ones land while the kernel
 * is still bringing up its CPUs; the later ones stop a guest that waits for
 * its root device on every CPU. */
constexpr unsigned int stop_delays_ms[]{0, 20, 60, 150, 400, 1000};
constexpr auto child_deadline = 30s;
constexpr auto linger_after_run = 300ms;
constexpr useconds_t widened_release_us = 50'000;

std::atomic_bool stopping;
std::atomic_bool kernel_called;
thread_local bool broadcast_before_unlock;

/* This executable's own code binds to the definitions below as well, so they
 * find the host's lazily, possibly before main(). */
template <typename Function>
Function host_function(std::atomic<Function> &cache, const char *name)
{
	Function function = cache.load(std::memory_order_acquire);
	if (function == nullptr) {
		function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
		if (function == nullptr)
			std::abort();
		cache.store(function, std::memory_order_release);
	}
	return function;
}

std::atomic<int (*)(pthread_mutex_t *)> host_mutex_unlock;
std::atomic<int (*)(pthread_cond_t *)> host_cond_broadcast;

bool on_vcpu_carrier()
{
	char name[64]{};
	if (pthread_getname_np(pthread_self(), name, sizeof name) != 0)
		return false;
	const std::string_view thread{name};
	return thread.starts_with("uml-vcpu-") && thread.ends_with("-carrier");
}

/* The emulator's own code reaches these definitions too, so only a vCPU
 * carrier's call shows that the run has passed admission into the kernel. */
void note_kernel_call()
{
	if (!kernel_called.load(std::memory_order_relaxed) && on_vcpu_carrier())
		kernel_called.store(true, std::memory_order_release);
}

std::size_t thread_count()
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return 0;
	for (mach_msg_type_number_t index = 0; index < count; ++index)
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(*threads));
	return count;
}

std::vector<std::byte> read_file(const char *path)
{
	FILE *file = std::fopen(path, "rb");
	if (file == nullptr)
		throw std::runtime_error{std::string{"cannot open "} + path};
	std::vector<std::byte> bytes;
	std::byte buffer[1 << 16];
	std::size_t count;
	while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0)
		bytes.insert(bytes.end(), buffer, buffer + count);
	const bool failed = std::ferror(file) != 0;
	std::fclose(file);
	if (failed || bytes.empty())
		throw std::runtime_error{std::string{"cannot read "} + path};
	return bytes;
}

/* The kernel's handler has replaced the default action, which would end the
 * process. */
bool kernel_handles_alarm()
{
	struct sigaction current {};
	return sigaction(SIGALRM, nullptr, &current) == 0 &&
		current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN;
}

enum class ForeignAlarm { not_sent, refused, handled };

/* The foreign alarm and the stop exclude each other: the run end restores
 * SIGALRM's default action, which would end the process if the alarm came
 * after it. Whichever claims the gate first goes ahead, and a stop waits for
 * an alarm already under way. */
enum AlarmGate : int { gate_open, alarm_sending, alarm_done, stop_claimed };

/* Sends SIGALRM to this non-vCPU thread with the signal unblocked. A
 * self-directed signal is handled before pthread_kill() returns, so the mask
 * then shows whether the kernel refused it. */
ForeignAlarm take_foreign_alarm(std::atomic_int &gate,
	const std::atomic_bool &returned)
{
	for (;;) {
		if (kernel_handles_alarm()) {
			int expected = gate_open;
			if (!gate.compare_exchange_strong(expected, alarm_sending,
				    std::memory_order_acq_rel))
				return ForeignAlarm::not_sent;
			break;
		}
		if (gate.load(std::memory_order_acquire) == stop_claimed ||
		    returned.load(std::memory_order_acquire))
			return ForeignAlarm::not_sent;
		std::this_thread::sleep_for(1ms);
	}
	sigset_t timer;
	sigemptyset(&timer);
	sigaddset(&timer, SIGALRM);
	if (pthread_sigmask(SIG_UNBLOCK, &timer, nullptr) != 0)
		std::_Exit(10);
	(void)pthread_kill(pthread_self(), SIGALRM);
	sigset_t now;
	if (pthread_sigmask(SIG_SETMASK, nullptr, &now) != 0)
		std::_Exit(10);
	const bool refused = sigismember(&now, SIGALRM) == 1;
	if (!refused && pthread_sigmask(SIG_BLOCK, &timer, nullptr) != 0)
		std::_Exit(10);
	gate.store(alarm_done, std::memory_order_release);
	return refused ? ForeignAlarm::refused : ForeignAlarm::handled;
}

/* One guest, stopped after the given delay. Exits 0 only when the run
 * returned 0, the kernel refused any tick a foreign thread took, and nothing
 * faulted while the process lingered after it. */
[[noreturn]] void run_child(const char *kernel_path, const char *disk_path,
	unsigned int delay_ms)
{
	const auto kernel = read_file(kernel_path);
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.kernel_image = kernel.data(),
		.kernel_image_size = kernel.size(),
	};
	uml_emulator *created = nullptr;
	if (uml_emulator_create(&config, &created) != UML_EMULATOR_OK)
		std::_Exit(10);
	/* The stopper thread inherits this mask. */
	sigset_t timer;
	sigemptyset(&timer);
	sigaddset(&timer, SIGALRM);
	if (pthread_sigmask(SIG_BLOCK, &timer, nullptr) != 0)
		std::_Exit(10);
	const std::string disk = std::string{"ubd0c="} + disk_path;
	const char *arguments[]{"uml-run-end", disk.c_str(), "root=/dev/ubdz",
		"rootwait", "mem=256M", "ncpus=4", "init=/bin/sh"};
	std::atomic_bool returned{};
	std::atomic_int gate{gate_open};
	auto foreign_alarm = ForeignAlarm::not_sent;
	std::thread alarm_taker{[&] {
		foreign_alarm = take_foreign_alarm(gate, returned);
	}};
	std::thread stopper{[&] {
		while (!kernel_called.load(std::memory_order_acquire) &&
		       !returned.load(std::memory_order_acquire))
			std::this_thread::sleep_for(1ms);
		std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms});
		int expected = gate_open;
		if (!gate.compare_exchange_strong(expected, stop_claimed,
			    std::memory_order_acq_rel))
			while (gate.load(std::memory_order_acquire) != alarm_done)
				std::this_thread::sleep_for(1ms);
		/* A request made before the run registers is not inherited, so keep
		 * asking until the run returns. */
		stopping.store(true, std::memory_order_release);
		while (!returned.load(std::memory_order_acquire)) {
			uml_emulator_request_stop(created);
			std::this_thread::sleep_for(20ms);
		}
	}};
	const int result = uml_emulator_run(created,
		static_cast<int>(std::size(arguments)), arguments, nullptr);
	returned.store(true, std::memory_order_release);
	stopper.join();
	alarm_taker.join();
	std::fprintf(stderr, "run-end-test: run returned %d\n", result);
	static constexpr const char *alarm_outcome[]{"not sent", "refused",
		"run on the foreign thread"};
	std::fprintf(stderr, "run-end-test: foreign alarm %s\n",
		alarm_outcome[static_cast<int>(foreign_alarm)]);
	std::this_thread::sleep_for(linger_after_run);
	if (result != 0)
		std::_Exit(11);
	if (foreign_alarm == ForeignAlarm::handled)
		std::_Exit(13);
	/* Only this thread may remain. */
	const std::size_t threads = thread_count();
	if (threads != 1) {
		std::fprintf(stderr, "run-end-test: %zu threads remain\n", threads);
		std::_Exit(12);
	}
	std::_Exit(0);
}

bool log_contains(const std::string &path, std::string_view text)
{
	std::ifstream file{path};
	std::stringstream contents;
	contents << file.rdbuf();
	return contents.str().find(text) != std::string::npos;
}

std::string describe(int status)
{
	if (WIFEXITED(status))
		return "exit " + std::to_string(WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		return std::string{"signal "} + strsignal(WTERMSIG(status));
	return "status " + std::to_string(status);
}

int run_driver(const char *self, const char *kernel_path,
	const std::string &scratch, unsigned int iterations)
{
	if (::mkdir(scratch.c_str(), 0700) != 0 && errno != EEXIST)
		throw std::runtime_error{"cannot create " + scratch};
	const std::string disk = scratch + "/empty-disk.img";
	const int descriptor = ::open(disk.c_str(),
		O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (descriptor < 0 || ::ftruncate(descriptor, 16 << 20) != 0)
		throw std::runtime_error{"cannot create " + disk};
	::close(descriptor);

	unsigned int failures = 0;
	unsigned int refused_alarms = 0;
	for (unsigned int iteration = 0; iteration < iterations; ++iteration) {
		const unsigned int delay =
			stop_delays_ms[iteration % std::size(stop_delays_ms)];
		const std::string log = scratch + "/attempt-" +
			std::to_string(iteration) + ".log";
		const std::string delay_text = std::to_string(delay);
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
			"/dev/null", O_RDONLY, 0);
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
			log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
		posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
			STDERR_FILENO);
		const char *child_arguments[]{self, "--child", kernel_path,
			disk.c_str(), delay_text.c_str(), nullptr};
		pid_t child = 0;
		const int spawned = posix_spawn(&child, self, &actions, nullptr,
			const_cast<char *const *>(child_arguments), environ);
		posix_spawn_file_actions_destroy(&actions);
		if (spawned != 0)
			throw std::runtime_error{"cannot spawn an attempt"};

		const auto deadline = Clock::now() + child_deadline;
		int status = 0;
		pid_t waited = 0;
		while ((waited = ::waitpid(child, &status, WNOHANG)) == 0 &&
		       Clock::now() < deadline)
			std::this_thread::sleep_for(20ms);
		if (waited == 0) {
			(void)::kill(child, SIGKILL);
			(void)::waitpid(child, &status, 0);
			std::fprintf(stderr, "attempt %u (stop after %u ms) did not "
				"end within %lld s; see %s\n", iteration, delay,
				static_cast<long long>(child_deadline.count()),
				log.c_str());
			++failures;
			continue;
		}
		if (log_contains(log, "run-end-test: foreign alarm refused"))
			++refused_alarms;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			continue;
		std::fprintf(stderr, "attempt %u (stop after %u ms) ended with "
			"%s; see %s\n", iteration, delay, describe(status).c_str(),
			log.c_str());
		++failures;
	}
	std::fprintf(stderr, "%u of %u stopped runs ended cleanly\n",
		iterations - failures, iterations);
	/* Early stops end a run before the kernel handles SIGALRM, but the later
	 * ones must have checked a foreign thread's tick. */
	std::fprintf(stderr, "%u runs refused a foreign thread's SIGALRM\n",
		refused_alarms);
	if (refused_alarms == 0)
		return EXIT_FAILURE;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

extern "C" int pthread_cond_broadcast(pthread_cond_t *condition)
{
	const int result = host_function(host_cond_broadcast,
		"pthread_cond_broadcast")(condition);
	note_kernel_call();
	if (stopping.load(std::memory_order_acquire) && on_vcpu_carrier())
		broadcast_before_unlock = true;
	return result;
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	const int result = host_function(host_mutex_unlock,
		"pthread_mutex_unlock")(mutex);
	note_kernel_call();
	if (broadcast_before_unlock) {
		broadcast_before_unlock = false;
		(void)::usleep(widened_release_us);
	}
	return result;
}

int main(int argc, char **argv)
{
	try {
		if (argc == 5 && std::string_view{argv[1]} == "--child")
			run_child(argv[2], argv[3],
				static_cast<unsigned int>(std::stoul(argv[4])));
		if (argc == 4) {
			const auto iterations =
				static_cast<unsigned int>(std::stoul(argv[3]));
			if (iterations == 0)
				throw std::runtime_error{"no attempts requested"};
			return run_driver(argv[0], argv[1], argv[2], iterations);
		}
		std::fputs("usage: uml_run_end_stop_tests KERNEL SCRATCH "
			"ITERATIONS\n", stderr);
		return EXIT_FAILURE;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return EXIT_FAILURE;
	}
}
