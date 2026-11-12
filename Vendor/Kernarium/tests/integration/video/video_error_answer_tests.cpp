// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A Virtio video buffer the device answers as failed.
 *
 * The driver returns a capture buffer the device failed to a client flagged
 * V4L2_BUF_FLAG_ERROR, and it must name the picture the device could not
 * deliver by the timestamp of the answer, as a buffer holding a picture
 * does. A host drops a picture only when its decoder or encoder does, which a
 * test cannot make happen, so the guest's decoder is served by the production
 * device and transport behind a shim (tests/support/vhost_user) that answers
 * the capture buffer the guest queues as a failed one, naming a timestamp.
 *
 * The guest's /init (video_error_answer_init.cpp) sets the decoder's formats,
 * queues one capture buffer with another timestamp, starts both queues and
 * dequeues the buffer. It must come back flagged as an error, empty, and with
 * the timestamp the shim named.
 *
 * A process boots one guest, so the run is a child process of this one, with
 * its console in a log the parent then checks.
 *
 *   uml_video_error_answer_tests KERNEL INIT SCRATCH
 */
#include "uml/emulator.h"

#include "boot_initramfs.hpp"
#include "delayed_answer_device.hpp"
#include "in_process_vhost_user_service.hpp"
#include "video_capabilities.hpp"
#include "video_decoder.hpp"
#include "virtio_video_device.hpp"

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

constexpr auto child_deadline = 180s;
/* The timestamp the shim names on the failed buffer: 3.141592 s. */
constexpr std::uint64_t failed_timestamp = 3141592000;
constexpr std::string_view failures[]{
	"WARNING:", "BUG ", "Oops", "Call Trace:", "Kernel panic",
	"Darwin UML panic", "Kernel mode fault"};

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
	const std::string text = "error-answer: " + line + "\n";
	(void)!::write(STDOUT_FILENO, text.data(), text.size());
}

/* Answers a decoded-picture buffer the guest queues as failed, naming the
 * timestamp of a picture the device could not deliver. */
std::optional<std::vector<std::uint8_t>> fail_picture_buffer(
	std::uint32_t queue, std::span<const std::uint8_t> request)
{
	if (queue != 0 || request.size() < sizeof(virtio_video_resource_queue))
		return std::nullopt;
	const auto queued =
		uml::virtio::fromBytes<virtio_video_resource_queue>(request);
	if (queued.cmd_type != VIRTIO_VIDEO_CMD_RESOURCE_QUEUE ||
	    queued.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT)
		return std::nullopt;
	log_line("failed buffer stream=" + std::to_string(queued.stream_id) +
		 " resource=" + std::to_string(queued.resource_id));
	virtio_video_resource_queue_resp answer{};
	answer.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, queued.stream_id};
	answer.flags = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
	answer.timestamp = failed_timestamp;
	return uml::virtio::toBytes(answer);
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
				[](std::uint32_t, std::span<const std::uint8_t>)
					-> std::optional<std::chrono::milliseconds> {
					return std::nullopt;
				},
				[](std::string_view message) {
					log_line(std::string{"shim "} +
						 std::string{message});
				},
				fail_picture_buffer);
		}};
	const std::string device = uml::gpu::virtioDeviceArgument(
		service.takeConnectionName(), uml::video::deviceId);
	const std::string ubd = "ubd0c=" + disk;
	const std::string initrd_argument = "initrd=" + initrd;
	const char *arguments[]{"uml-video-error-answer", ubd.c_str(),
		initrd_argument.c_str(), "mem=512M", "ncpus=2", device.c_str()};
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
	for (const auto expected : {"error-answer: failed buffer stream=",
		     "VIDEO_INIT DQBUF raw ok",
		     "VIDEO_INIT failed buffer error=1 empty=1 timestamp=3.141592 named",
		     "VIDEO_INIT done"})
		if (log.find(expected) == std::string::npos)
			problems.push_back(std::string{"missing \""} + expected + "\"");
	if (problems.empty()) {
		std::puts("a failed capture buffer names the picture it answers");
		return 0;
	}
	for (const auto &problem : problems)
		std::fprintf(stderr, "guest-video-error-answer: %s\n", problem.c_str());
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
		std::fprintf(stderr, "guest-video-error-answer: %s\n", error.what());
		return 1;
	}
}
