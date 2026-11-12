// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A Virtio video command the device answers after the driver gave up on it.
 *
 * The decoder driver waits five seconds for each synchronous command. On
 * Darwin the in-process device answers a command before the kick that sent it
 * returns, so only a device that queues its work answers later; this test
 * makes one by serving the guest's decoder with the production device and
 * transport behind a delayed-answer shim (tests/support/vhost_user). The
 * shim answers the first stream-parameter query 6.5 s late and never answers
 * the second.
 *
 * The guest's /init (video_late_answer_init.cpp) opens the decoder three
 * times and has SLUB validate the driver's command cache after the late
 * answer. The first two opens must fail with ETIMEDOUT, the third must
 * succeed, and the guest must power off. The late answer must land in memory
 * the driver still keeps for the command, so slab_debug=FZPU reports nothing;
 * every stream a failed open made must be destroyed again; and the command
 * that was never answered must stay allocated until the driver's unbind takes
 * it back from the device, which destroying the command cache then checks.
 *
 * A process boots one guest, so the run is a child process of this one, with
 * its console in a log the parent then checks.
 *
 *   uml_video_late_answer_tests KERNEL INIT SCRATCH
 */
#include "uml/emulator.h"

#include "boot_initramfs.hpp"
#include "delayed_answer_device.hpp"
#include "in_process_vhost_user_service.hpp"
#include "video_capabilities.hpp"
#include "video_decoder.hpp"
#include "virtio_video_device.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <optional>
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

constexpr auto first_delay = 6500ms;
constexpr auto child_deadline = 180s;
constexpr std::string_view failures[]{
	"WARNING:", "BUG ", "Oops", "Call Trace:", "Kernel panic",
	"Darwin UML panic", "Kernel mode fault", "Poison overwritten",
	"Redzone", "Object already free", "Objects remaining"};

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
	const std::string text = "late-answer: " + line + "\n";
	(void)!::write(STDOUT_FILENO, text.data(), text.size());
}

const char *command_name(std::uint32_t type)
{
	switch (type) {
	case VIRTIO_VIDEO_CMD_STREAM_CREATE:
		return "STREAM_CREATE";
	case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
		return "STREAM_DESTROY";
	case VIRTIO_VIDEO_CMD_GET_PARAMS:
		return "GET_PARAMS";
	default:
		return nullptr;
	}
}

/* Logs the stream commands and holds back the first two parameter queries:
 * one for a while, one for good. */
uml::testing::DelayedAnswerDevice::Policy late_parameters()
{
	auto held = std::make_shared<int>(0);
	return [held](std::uint32_t queue, std::span<const std::uint8_t> request)
		       -> std::optional<std::chrono::milliseconds> {
		if (queue != 0 || request.size() < sizeof(virtio_video_cmd_hdr))
			return std::nullopt;
		const auto header =
			uml::virtio::fromBytes<virtio_video_cmd_hdr>(request);
		if (const char *name = command_name(header.type))
			log_line(std::string{name} + " stream=" +
				 std::to_string(header.stream_id));
		if (header.type != VIRTIO_VIDEO_CMD_GET_PARAMS || *held >= 2)
			return std::nullopt;
		return (*held)++ == 0 ? first_delay :
			uml::testing::DelayedAnswerDevice::never;
	};
}

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
		"Virtio video", "uml-video",
		[] {
			return std::make_unique<uml::testing::DelayedAnswerDevice>(
				std::make_unique<uml::video::VirtioVideoDevice>(
					uml::platform::videoDecodeCapabilities(),
					uml::platform::makeVideoDecoder),
				late_parameters(),
				[](std::string_view message) {
					log_line(std::string{"shim "} +
						 std::string{message});
				});
		}};
	const std::string device = uml::gpu::virtioDeviceArgument(
		service.takeConnectionName(), uml::video::deviceId);
	const std::string ubd = "ubd0c=" + disk;
	const std::string initrd_argument = "initrd=" + initrd;
	const char *arguments[]{"uml-video-late-answer", ubd.c_str(),
		initrd_argument.c_str(), "mem=512M", "ncpus=2",
		"slab_debug=FZPU", device.c_str()};
	const int result = uml_emulator_run(created,
		static_cast<int>(std::size(arguments)), arguments, nullptr);
	service.stop();
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

/* Every stream a create made is destroyed again before the guest stops. The
 * driver reuses the ID of a stream it gave up, so count each one. */
std::string unbalanced_streams(const std::string &log)
{
	const std::regex command{"late-answer: (STREAM_CREATE|STREAM_DESTROY) stream=([0-9]+)"};
	std::vector<std::string> open;
	for (std::sregex_iterator match{log.begin(), log.end(), command}, end;
	     match != end; ++match) {
		const std::string stream = (*match)[2];
		if ((*match)[1] == "STREAM_CREATE") {
			open.push_back(stream);
			continue;
		}
		if (const auto made = std::find(open.begin(), open.end(), stream);
		    made != open.end())
			open.erase(made);
	}
	std::string streams;
	for (const auto &stream : open)
		streams += " " + stream;
	return streams;
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
	for (const auto expected : {"VIDEO_INIT open1 errno=110",
		     "VIDEO_INIT late-answer validated", "VIDEO_INIT open2 errno=110",
		     "VIDEO_INIT open3 ok", "VIDEO_INIT final validated",
		     "VIDEO_INIT held objects=1", "VIDEO_INIT unbound",
		     "VIDEO_INIT cache destroyed", "VIDEO_INIT done",
		     "late-answer: shim delivered a held answer",
		     "late-answer: shim reset dropped 1 held answer(s)"})
		if (log.find(expected) == std::string::npos)
			problems.push_back(std::string{"missing \""} + expected + "\"");
	if (const auto streams = unbalanced_streams(log); !streams.empty())
		problems.push_back("streams never destroyed:" + streams);
	if (problems.empty()) {
		std::puts("late answers reached only memory the driver kept for them");
		return 0;
	}
	for (const auto &problem : problems)
		std::fprintf(stderr, "guest-video-late-answer: %s\n", problem.c_str());
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
		std::fprintf(stderr, "guest-video-late-answer: %s\n", error.what());
		return 1;
	}
}
