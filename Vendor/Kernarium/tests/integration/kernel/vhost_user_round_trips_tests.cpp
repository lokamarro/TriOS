// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synchronous vhost-user round trips that two vCPUs start at once.
 *
 * Every frontend request on a vhost-user socket but a kick to a GPU or sound
 * device has a reply, and nothing but order tells the frontend which request
 * a reply answers, so a frontend may send its next request only after it has
 * read the previous reply. With in-band notifications any other kick is such
 * a round trip too, and the guest kicks from any vCPU while another may be
 * reading the device's configuration. A kick sent while a GET_CONFIG reply
 * was still on its way took that reply as its acknowledgement and retired a
 * healthy GPU device in the middle of a display change. The console's kick
 * must also stay a round trip that serves the device's requests: its writer
 * spins on the output queue with interrupts off, and the device reads no
 * kick until the guest has answered the configuration change it sent.
 *
 * The device here is a single-port Virtio console, served in-process by the
 * production transport, that changes its size every few tens of
 * milliseconds and answers each configuration read late, as a busy backend
 * may. Each change sends the guest's console driver to read the new size
 * while the guest's /init (vhost_user_round_trips_init.cpp) keeps a writer
 * on every vCPU kicking the console's output queue. The console must stay
 * bound throughout, the guest must report no failed round trip and no lost
 * backend, the transport no broken connection, and the guest must power off.
 *
 * A process boots one guest, so the run is a child process of this one, with
 * its console in a log the parent then checks.
 *
 *   uml_vhost_user_round_trips_tests KERNEL INIT SCRATCH
 */
#include "uml/emulator.h"

#include "boot_initramfs.hpp"
#include "in_process_vhost_user_service.hpp"
#include "vhost_user_device.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <pthread.h>
#include <regex>
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

/* A configuration read is answered this late, so a request another vCPU
 * sends meanwhile is on the socket before the reply. */
constexpr auto read_delay = 10ms;
/* The console changes its size this often. */
constexpr auto change_interval = 30ms;
constexpr auto child_deadline = 240s;
/* What a run must at least have done to have raced anything. */
constexpr long minimum_changes = 20;
constexpr long minimum_reads = 40;
constexpr long minimum_writes = 100;

constexpr std::uint32_t console_device_id = 3;
constexpr std::uint64_t console_feature_size = 1ULL << 0U;
constexpr std::uint32_t console_output_queue = 1;

constexpr std::string_view failures[]{
	"WARNING:", "BUG ", "Oops", "Call Trace:", "Kernel panic",
	"Darwin UML panic", "Kernel mode fault",
	"the vhost-user backend is gone", "VHOST_USER_",
	"slave reports error",
	/* The console keeps changing its size until the guest powers off,
	 * which closes the connection with a change unanswered: that is not a
	 * broken connection either. */
	"vhost-user connection failed"};

/* The guest process, which a timed-out CTest's interrupt must not orphan. */
volatile sig_atomic_t guest_process = 0;

extern "C" void stop_guest(int signal)
{
	if (guest_process > 0)
		::kill(static_cast<pid_t>(guest_process), SIGKILL);
	::_exit(128 + signal);
}

std::vector<char> read_file(const char *path)
{
	std::ifstream file{path, std::ios::binary};
	if (!file)
		throw std::runtime_error{std::string{"cannot read "} + path};
	return {std::istreambuf_iterator<char>{file}, {}};
}

/* One whole line per write, so the transport thread's lines do not split
 * the kernel's. */
void log_line(const std::string &line)
{
	const std::string text = "round-trips: " + line + "\n";
	(void)!::write(STDOUT_FILENO, text.data(), text.size());
}

struct ConsoleConfiguration final {
	std::uint16_t columns {};
	std::uint16_t rows {};
	std::uint32_t maximumPorts {};
	std::uint32_t emergencyWrite {};
};

static_assert(sizeof(ConsoleConfiguration) == 12);

/* What the device saw, read by the child once the guest has stopped. */
struct Counters final {
	std::atomic_long reads {};
	std::atomic_long changes {};
	std::atomic_long writes {};
};

Counters counters;

/*
 * A single-port Virtio console whose size alternates every change_interval
 * and whose every configuration read is answered read_delay late. It drops
 * what the guest writes and never has input.
 */
class ResizingConsole final : public uml::virtio::VhostUserDevice {
public:
	ResizingConsole()
	{
		if (::pipe(wake_) != 0)
			throw std::runtime_error{"cannot create the resize pipe"};
		for (const int descriptor : wake_) {
			if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0 ||
			    ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
				throw std::runtime_error{"cannot configure the resize pipe"};
		}
		timer_ = std::thread{[this] { runTimer(); }};
	}

	~ResizingConsole() override
	{
		{
			std::lock_guard lock{mutex_};
			stopping_ = true;
		}
		stop_.notify_all();
		timer_.join();
		::close(wake_[0]);
		::close(wake_[1]);
	}

	ResizingConsole(const ResizingConsole &) = delete;
	ResizingConsole &operator=(const ResizingConsole &) = delete;

	[[nodiscard]] const char *name() const noexcept override
	{
		return "Resizing console";
	}

	[[nodiscard]] std::uint64_t features() const noexcept override
	{
		return console_feature_size;
	}

	[[nodiscard]] std::size_t queueCount() const noexcept override
	{
		return 2;
	}

	[[nodiscard]] std::vector<std::uint8_t> configuration() const override
	{
		std::this_thread::sleep_for(read_delay);
		counters.reads.fetch_add(1, std::memory_order_relaxed);
		const bool wide = (generation_.load(std::memory_order_acquire) & 1U) != 0;
		return uml::virtio::toBytes(ConsoleConfiguration{
			.columns = static_cast<std::uint16_t>(wide ? 132 : 80),
			.rows = static_cast<std::uint16_t>(wide ? 43 : 25),
		});
	}

	void writeConfiguration(std::uint32_t, std::uint32_t,
		std::span<const std::uint8_t>) override
	{
	}

	[[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
		std::uint32_t index, std::uint64_t,
		std::span<const std::uint8_t> request, std::size_t) override
	{
		if (index != console_output_queue)
			return {};
		counters.writes.fetch_add(1, std::memory_order_relaxed);
		(void)request;
		return {.action = uml::virtio::VhostUserQueueAction::complete,
			.response = {}};
	}

	void reset() noexcept override {}

	[[nodiscard]] int eventDescriptor() const noexcept override
	{
		return wake_[0];
	}

	void drainEventSource() override
	{
		char bytes[64];
		while (::read(wake_[0], bytes, sizeof(bytes)) > 0)
			;
	}

	[[nodiscard]] bool configChangePending() const noexcept override
	{
		return generation_.load(std::memory_order_acquire) !=
			signalled_.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool claimConfigChange() noexcept override
	{
		claimed_ = generation_.load(std::memory_order_acquire);
		return claimed_ != signalled_.load(std::memory_order_acquire);
	}

	void clearConfigChange() noexcept override
	{
		signalled_.store(claimed_, std::memory_order_release);
		counters.changes.fetch_add(1, std::memory_order_relaxed);
	}

private:
	void runTimer()
	{
		std::unique_lock lock{mutex_};
		while (!stop_.wait_for(lock, change_interval,
			[this] { return stopping_; })) {
			generation_.fetch_add(1, std::memory_order_acq_rel);
			const char byte = 1;
			(void)!::write(wake_[1], &byte, 1);
		}
	}

	int wake_[2] {-1, -1};
	std::atomic_uint32_t generation_ {};
	std::atomic_uint32_t signalled_ {};
	std::uint32_t claimed_ {};
	std::mutex mutex_;
	std::condition_variable stop_;
	bool stopping_ {};
	std::thread timer_;
};

[[noreturn]] void run_child(const char *kernel_path, const char *init_path,
	const std::string &scratch)
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
	/* Like every embedder, keep the kernel's early SIGALRM off this
	 * process's own threads; the service thread inherits the mask. */
	sigset_t timer;
	sigemptyset(&timer);
	sigaddset(&timer, SIGALRM);
	if (pthread_sigmask(SIG_BLOCK, &timer, nullptr) != 0)
		std::_Exit(10);

	const std::string initrd = scratch + "/initrd";
	const std::string disk = scratch + "/empty-disk.img";
	uml::boot::InitramfsArchive archive;
	archive.addHostFile("/init", init_path, 0755);
	(void)::unlink(initrd.c_str());
	uml::boot::writeNewFile(initrd, archive.serialize());
	const int descriptor = ::open(disk.c_str(),
		O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (descriptor < 0 || ::ftruncate(descriptor, 16 << 20) != 0)
		std::_Exit(10);
	::close(descriptor);

	uml::gpu::InProcessVhostUserService service{
		"Resizing console", "uml-round-trips",
		[] { return std::make_unique<ResizingConsole>(); }};
	const std::string device = uml::gpu::virtioDeviceArgument(
		service.takeConnectionName(), console_device_id);
	const std::string ubd = "ubd0c=" + disk;
	const std::string initrd_argument = "initrd=" + initrd;
	const char *arguments[]{"uml-vhost-user-round-trips", ubd.c_str(),
		initrd_argument.c_str(), "mem=512M", "ncpus=4", device.c_str()};
	const int result = uml_emulator_run(created,
		static_cast<int>(std::size(arguments)), arguments, nullptr);
	service.stop();
	log_line("reads=" + std::to_string(counters.reads.load()) +
		" changes=" + std::to_string(counters.changes.load()) +
		" writes=" + std::to_string(counters.writes.load()));
	log_line("run returned " + std::to_string(result));
	std::_Exit(result == 0 ? 0 : 11);
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
	const char *init_path, const std::string &scratch)
{
	if (::mkdir(scratch.c_str(), 0700) != 0 && errno != EEXIST)
		throw std::runtime_error{"cannot create " + scratch};
	const std::string log_path = scratch + "/guest.log";
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
		O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
		log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
		STDERR_FILENO);
	const char *child_arguments[]{self, "--child", kernel_path, init_path,
		scratch.c_str(), nullptr};
	pid_t child = 0;
	const int spawned = posix_spawn(&child, self, &actions, nullptr,
		const_cast<char *const *>(child_arguments), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0)
		throw std::runtime_error{"cannot start the guest process"};
	guest_process = child;
	std::signal(SIGINT, stop_guest);
	std::signal(SIGTERM, stop_guest);

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + child_deadline;
	bool timed_out = false;
	while (::waitpid(child, &status, WNOHANG) == 0) {
		if (std::chrono::steady_clock::now() > deadline) {
			timed_out = true;
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
			break;
		}
		std::this_thread::sleep_for(100ms);
	}

	std::ifstream file{log_path};
	std::stringstream contents;
	contents << file.rdbuf();
	const std::string log = contents.str();
	std::vector<std::string> problems;
	if (timed_out)
		problems.push_back("the guest did not stop within " +
			std::to_string(child_deadline.count()) + " s");
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		problems.push_back("the guest process ended with " + describe(status));
	for (const auto failure : failures)
		if (log.find(failure) != std::string::npos)
			problems.push_back("the guest printed \"" +
				std::string{failure} + "\"");
	for (const auto expected : {"ROUND_TRIPS writers started",
		     "ROUND_TRIPS writers finished=4 succeeded=4",
		     "ROUND_TRIPS bound", "ROUND_TRIPS done"})
		if (log.find(expected) == std::string::npos)
			problems.push_back(std::string{"missing \""} + expected + "\"");
	const std::regex totals{
		"round-trips: reads=([0-9]+) changes=([0-9]+) writes=([0-9]+)"};
	std::smatch match;
	if (!std::regex_search(log, match, totals)) {
		problems.push_back("the device reported no totals");
	} else if (std::stol(match[1]) < minimum_reads ||
		   std::stol(match[2]) < minimum_changes ||
		   std::stol(match[3]) < minimum_writes) {
		problems.push_back("too little traffic to race: " + match.str());
	}
	if (problems.empty()) {
		std::printf("every round trip kept its own reply (%s)\n",
			match.str().c_str());
		return 0;
	}
	for (const auto &problem : problems)
		std::fprintf(stderr, "guest-vhost-user-round-trips: %s\n",
			problem.c_str());
	std::fprintf(stderr, "--- %s ---\n%s\n", log_path.c_str(),
		log.size() > 20000 ? log.substr(log.size() - 20000).c_str() :
		log.c_str());
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	try {
		if (argc == 5 && std::string_view{argv[1]} == "--child")
			run_child(argv[2], argv[3], argv[4]);
		if (argc != 4) {
			std::fprintf(stderr, "usage: %s KERNEL INIT SCRATCH\n", argv[0]);
			return 2;
		}
		return run_driver(argv[0], argv[1], argv[2], argv[3]);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "guest-vhost-user-round-trips: %s\n",
			error.what());
		return 1;
	}
}
