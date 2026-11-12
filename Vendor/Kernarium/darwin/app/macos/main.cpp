// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/in_process_service.hpp"
#include "audio/microphone_switch.hpp"
#include "audio/protocol.hpp"
#include "battery/in_process_service.hpp"
#include "battery/protocol.hpp"
#include "video/in_process_service.hpp"
#include "video/virtio_video_device.hpp"
#include "video/virtio_video_encoder_device.hpp"
#include "vsock/forwarder.hpp"
#include "vsock/in_process_service.hpp"
#include "vsock/protocol.hpp"
#include "console/in_process_service.hpp"
#include "console/protocol.hpp"
#include "gpu/in_process_service.hpp"
#include "gpu/mesa_shader_cache.hpp"
#include "gpu/metal_shader_cache.hpp"
#include "gpu/native_display_application.hpp"
#include "gpu/protocol.hpp"
#include "launcher.hpp"
#include "gpu_budget_plan.hpp"
#include "guest_memory_argument.hpp"
#include "options.hpp"
#include "platform/microphone_authorization.hpp"
#include "restart_loop_guard.hpp"
#include "sockets/socket_paths.hpp"
#include "statistics_report.hpp"
#include "uml/emulator.h"
#if UML_APP_HAS_SLIRP
#include "uml_slirp_net.h"
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <pthread.h>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <mach/arm/thread_status.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C" {
extern const unsigned char uml_elf_start[]
	__asm("section$start$__TEXT$__uml_elf");
extern const unsigned char uml_elf_end[]
	__asm("section$end$__TEXT$__uml_elf");
}

namespace {

constexpr char host_fault_prefix[] = "Darwin UML host signal ";
constexpr char host_fault_pc_prefix[] = " at pc=0x";
constexpr char host_fault_address_prefix[] = " address=0x";
constexpr std::size_t host_fault_hex_digits = sizeof(std::uintptr_t) * 2U;
constexpr std::size_t host_fault_message_capacity = 128;
class AppNetwork final {
public:
	AppNetwork()
	{
#if UML_APP_HAS_SLIRP
		context_ = uml_slirp_net_create();
		if (context_ == nullptr)
			throw std::bad_alloc{};
#endif
	}

	~AppNetwork() { shutdown(); }

	AppNetwork(const AppNetwork &) = delete;
	AppNetwork &operator=(const AppNetwork &) = delete;

	[[nodiscard]] uml_emulator_network_callbacks callbacks() noexcept
	{
#if UML_APP_HAS_SLIRP
		return {
			.struct_size = sizeof(uml_emulator_network_callbacks),
			.context = context_,
			.open = uml_slirp_net_open,
			.irq_fd = uml_slirp_net_irq_fd,
			.read = uml_slirp_net_read,
			.write = uml_slirp_net_write,
		};
#else
		return {};
#endif
	}

	/* Registered before the guest brings the interface up, so this only
	 * validates the specification; a host port that cannot be claimed is
	 * reported by the provider when it installs the forward. */
	[[nodiscard]] int forward(const std::string &specification) noexcept
	{
#if UML_APP_HAS_SLIRP
		return context_ == nullptr ?
			-EINVAL :
			uml_slirp_net_add_forward(context_, specification.c_str());
#else
		(void)specification;
		return -ENOTSUP;
#endif
	}

	/* The link goes down with the guest that used it: its connections end,
	 * as they would if the cable were pulled, and a connection the guest
	 * never closed does not keep the host side open for the next guest. The
	 * new link is up at once, so the host forwards listen again while the
	 * next guest boots; the guest's own open() then finds it open. The
	 * provider replays the recorded forwards onto the new instance and keeps
	 * the descriptors the guest's IRQ registration borrows. */
	void reset_link() noexcept
	{
#if UML_APP_HAS_SLIRP
		if (context_ == nullptr)
			return;
		uml_slirp_net_stop(context_);
		unsigned char mac[6]{};
		unsigned int mtu{};
		if (const int opened = uml_slirp_net_open(context_, mac, &mtu);
		    opened < 0)
			std::fprintf(stderr,
				"Unable to bring the user network up again: %d\n",
				opened);
#endif
	}

	void shutdown() noexcept
	{
#if UML_APP_HAS_SLIRP
		if (context_ != nullptr) {
			uml_slirp_net_stop(context_);
			uml_slirp_net_destroy(context_);
			context_ = nullptr;
		}
#endif
	}

private:
	void *context_{};
};

[[nodiscard]] bool user_network_enabled() noexcept
{
#if UML_APP_HAS_SLIRP
	const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
	return disabled == nullptr || disabled[0] == '\0' ||
		disabled[0] == '0';
#else
	return false;
#endif
}

template<std::size_t Size>
char *append_host_fault_literal(char *output,
				const char (&literal)[Size]) noexcept
{
	for (std::size_t index = 0; index + 1U < Size; ++index)
		*output++ = literal[index];
	return output;
}

char *append_host_fault_signal(char *output, int signal) noexcept
{
	constexpr std::size_t decimal_digits =
		std::numeric_limits<unsigned int>::digits10 + 1U;
	char reversed[decimal_digits];
	std::size_t count = 0;
	unsigned int magnitude = static_cast<unsigned int>(signal);
	if (signal < 0) {
		*output++ = '-';
		magnitude = 0U - magnitude;
	}
	do {
		reversed[count++] = static_cast<char>('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude != 0U);
	while (count != 0U)
		*output++ = reversed[--count];
	return output;
}

char *append_host_fault_hex(char *output, std::uintptr_t value) noexcept
{
	constexpr char digits[] = "0123456789abcdef";
	for (std::size_t index = host_fault_hex_digits; index != 0U; --index) {
		const auto shift = (index - 1U) * 4U;
		*output++ = digits[(value >> shift) & 0xfU];
	}
	return output;
}

void host_fault(int signal, siginfo_t *info, void *context_pointer) noexcept
{
	auto &context = *static_cast<ucontext_t *>(context_pointer);
	const auto pc = static_cast<std::uintptr_t>(
		__darwin_arm_thread_state64_get_pc(context.uc_mcontext->__ss));
	const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
	static_assert((sizeof(host_fault_prefix) - 1U) +
			      (std::numeric_limits<unsigned int>::digits10 + 2U) +
			      (sizeof(host_fault_pc_prefix) - 1U) +
			      host_fault_hex_digits +
			      (sizeof(host_fault_address_prefix) - 1U) +
			      host_fault_hex_digits + 1U <=
		      host_fault_message_capacity);
	char message[host_fault_message_capacity];
	char *output = message;
	output = append_host_fault_literal(output, host_fault_prefix);
	output = append_host_fault_signal(output, signal);
	output = append_host_fault_literal(output, host_fault_pc_prefix);
	output = append_host_fault_hex(output, pc);
	output = append_host_fault_literal(output, host_fault_address_prefix);
	output = append_host_fault_hex(output, address);
	*output++ = '\n';
	(void)write(STDERR_FILENO, message,
		    static_cast<std::size_t>(output - message));
	/* This path never unwinds, so the bound socket paths would survive the
	 * process and block the next run on the same names. */
	::uml::device::remove_registered_socket_paths();
	_exit(128 + signal);
}

void install_host_faults()
{
	constexpr std::array signals{SIGABRT, SIGBUS, SIGSEGV, SIGILL};
	struct sigaction action {};
	action.sa_sigaction = host_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (const int signal : signals)
		if (sigaction(signal, &action, nullptr) < 0)
			std::abort();
}

/* A write to a socket whose peer has gone must fail with EPIPE, not kill the
 * process. The guest's kernel ignores SIGPIPE while it runs, but the library
 * hands the host's own disposition back when a run ends, so a launcher that
 * left it at the default would lose the process to the first host socket
 * written between two boots (libslirp writes its sockets with plain send()).
 * Set before the first run, it is the disposition every later run returns to. */
void ignore_broken_pipes()
{
	struct sigaction action {};
	action.sa_handler = SIG_IGN;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPIPE, &action, nullptr) < 0)
		std::abort();
}

/* Metal reads the whole index of the process's shader cache at its first
 * compile, and conformance campaigns grow that index without bound. Cache the
 * guest's shaders only while the index is small; a removal is the only event
 * worth a line. */
void bound_metal_shader_cache()
{
	std::error_code error;
	const auto outcome = uml::gpu::bound_own_metal_shader_cache(error);
	if (!outcome)
		return;
	if (error) {
		std::fprintf(stderr,
			"Unable to remove the Metal shader cache %s: %s\n",
			outcome->directory.c_str(), error.message().c_str());
		return;
	}
	if (outcome->action ==
	    uml::gpu::MetalShaderCacheOutcome::Action::removed)
		std::fprintf(stderr,
			"Removed the Metal shader cache %s: index %.1f MB over "
			"%.1f MB, data %.1f MB\n",
			outcome->directory.c_str(),
			static_cast<double>(outcome->size.index) / 1e6,
			static_cast<double>(
				uml::gpu::metal_shader_cache_index_limit) /
				1e6,
			static_cast<double>(outcome->size.data) / 1e6);
}

/* Mesa reads its disk cache's location and limit from the environment when
 * the host GPU's screen is created, on a worker thread. Place the cache below
 * the process's own cache directory, and bound it, here, before any thread
 * can read the environment. A line goes out only when the cache cannot be
 * placed, since Mesa then runs without one. */
void place_mesa_shader_cache()
{
	std::error_code error;
	const auto outcome = uml::gpu::configure_own_mesa_shader_cache(error);
	if (error)
		std::fprintf(stderr,
			"Unable to create the Mesa shader cache %s: %s\n",
			outcome.directory.c_str(), error.message().c_str());
}

struct HostGpuOptions {
	bool enabled {};
	uml::gpu::DisplayMode display_mode {uml::gpu::DisplayMode::vnc};
	bool vnc_option_specified {};
	bool auto_resize_guest {true};
	bool auto_resize_guest_specified {};
	std::string vnc_bind_address {"127.0.0.1"};
	std::uint16_t vnc_port {5900};
	std::string vnc_password;
	std::uint32_t width {1280};
	std::uint32_t height {720};
	std::uint32_t dots_per_inch {};
	uml::gpu::OffscreenDisplayOptions offscreen;
	bool offscreen_option_specified {};

	[[nodiscard]] bool operator==(const HostGpuOptions &) const = default;
};

struct PreparedArguments {
	HostGpuOptions gpu;
	bool audio {};
	uml::audio::OutputMode audio_output {uml::audio::OutputMode::host};
	bool audio_input_host {};
	bool battery {};
	bool video {};
	bool video_encoder {};
	/* --gpu-budget-app=, --gpu-budget-engine= and --gpu-budget-headroom=:
	 * A, E_rest and S of the host GPU's fixed budget
	 * (platform/gpu_budget.hpp). */
	uml::emulator::GpuBudgetParameters gpu_budget;
	std::string console_socket;
	std::string serial_socket;
	std::vector<std::string> net_forwards;
	/* --vsock-forward=: loopback ports that reach guest vsock ports. Any
	 * forward adds the Virtio vsock device, last among the devices. */
	std::vector<uml::vsock::ForwardSpecification> vsock_forwards;
	/* --stats=<ms>: print one line of the running guest's statistics every
	 * interval, from the calls the iOS applications make; 0 prints none. */
	std::uint32_t stats_interval_ms {};
	std::vector<std::string> kernel;

	/* Whether `other` gives the emulator the same machine: the arguments it
	 * reads for the vCPU count, the guest memory, the engine and the JIT and
	 * gadget cache budgets. The budgets are published once per process and a
	 * restarted run asks for the value the first did. */
	[[nodiscard]] bool same_machine(const PreparedArguments &other) const
	{
		const auto machine = [](const std::vector<std::string> &arguments) {
			std::vector<std::string> selected;
			for (const auto &argument : arguments)
				for (const std::string_view prefix : {
					     "ncpus=", "mem=", "--execution-engine",
					     "--jit", "--gadget-", "--guest-",
					     "--memory-limit="})
					if (argument.starts_with(prefix)) {
						selected.push_back(argument);
						break;
					}
			return selected;
		};
		return machine(kernel) == machine(other.kernel);
	}

	/* Whether `other` asks for the same host devices. A guest that restarts
	 * keeps the devices the first boot created once (the display, the
	 * network, the forwards) and only its kernel arguments may differ. */
	[[nodiscard]] bool same_host_devices(
		const PreparedArguments &other) const
	{
		return std::tie(gpu, gpu_budget, audio, audio_output,
				audio_input_host, battery, video, video_encoder,
				console_socket, serial_socket, net_forwards,
				vsock_forwards) ==
			std::tie(other.gpu, other.gpu_budget, other.audio,
				 other.audio_output, other.audio_input_host,
				 other.battery, other.video, other.video_encoder,
				 other.console_socket, other.serial_socket,
				 other.net_forwards, other.vsock_forwards);
	}
};

void append_default_kernel_argument(std::vector<std::string> &arguments,
				    std::string_view prefix,
				    std::string_view value)
{
	for (const auto &argument : arguments)
		if (argument.starts_with(prefix))
			return;
	arguments.emplace_back(value);
}

template<typename Integer>
bool parse_host_integer(std::string_view argument, std::string_view prefix,
			Integer &destination, bool allow_zero = false)
{
	if (!argument.starts_with(prefix))
		return false;
	const auto value = argument.substr(prefix.size());
	Integer parsed {};
	const auto [end, error] = std::from_chars(
		value.data(), value.data() + value.size(), parsed);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    (!allow_zero && parsed == 0))
		throw std::invalid_argument("invalid host option: " +
					    std::string(argument));
	destination = parsed;
	return true;
}

/* Two positive whole numbers around `separator`, "2266x1374" or "800@15",
 * in the value of `argument`. */
void parse_host_pair(std::string_view argument, std::string_view value,
		     char separator, std::uint32_t &first,
		     std::uint32_t &second)
{
	const auto invalid = [&] {
		return std::invalid_argument("invalid host option: " +
					     std::string(argument));
	};
	const auto cut = value.find(separator);
	if (cut == std::string_view::npos)
		throw invalid();
	std::uint32_t parsed[2] {};
	const std::string_view parts[2] {value.substr(0, cut),
					 value.substr(cut + 1)};
	for (int i = 0; i < 2; ++i) {
		const auto [end, error] = std::from_chars(
			parts[i].data(), parts[i].data() + parts[i].size(),
			parsed[i]);
		if (error != std::errc {} || parts[i].empty() ||
		    end != parts[i].data() + parts[i].size() || parsed[i] == 0)
			throw invalid();
	}
	first = parsed[0];
	second = parsed[1];
}

/* The bytes a --gpu-budget-*= option states: a whole number with an optional
 * K, M or G suffix. */
std::uint64_t parse_gpu_budget_option(std::string_view argument,
				      std::string_view value)
{
	std::uint64_t bytes = 0;
	if (!uml::emulator::parse_gpu_budget_bytes(value, bytes))
		throw std::invalid_argument("invalid host option: " +
					    std::string(argument));
	return bytes;
}

/* Records the path of a socket option, which may be given once. */
void set_socket_path(std::string &destination, std::string_view option,
		     std::string_view value)
{
	if (!destination.empty())
		throw std::invalid_argument(std::string(option) +
					    " may be specified only once");
	if (value.empty())
		throw std::invalid_argument(std::string(option) +
					    " requires a path");
	destination = value;
}

/* The shortest --stats interval: a reading of the guest costs a few
 * microseconds, but every second one asks the guest for its memory. */
constexpr std::uint32_t minimum_stats_interval_ms = 100;

PreparedArguments prepare_host_arguments(
	const std::vector<std::string> &command_line,
	const uml::app::LaunchConfiguration &configuration)
{
	PreparedArguments prepared;
	prepared.kernel.reserve(command_line.size() + 3U);
	for (const auto &text : command_line) {
		const std::string_view argument = text;
		/* Whether `argument` is the option `prefix` names ("--x="), with
		 * what follows it left in `value`. */
		std::string_view value;
		const auto valued = [&](std::string_view prefix) {
			if (!argument.starts_with(prefix))
				return false;
			value = argument.substr(prefix.size());
			return true;
		};
		if (argument == "--gpu") {
			prepared.gpu.enabled = true;
		} else if (argument == "--audio") {
			prepared.audio = true;
		} else if (valued("--audio-output=")) {
			if (value == "host")
				prepared.audio_output = uml::audio::OutputMode::host;
			else if (value == "null")
				prepared.audio_output = uml::audio::OutputMode::null;
			else
				throw std::invalid_argument(
					"--audio-output must be host or null");
			prepared.audio = true;
		} else if (valued("--audio-input=")) {
			if (value == "silent")
				prepared.audio_input_host = false;
			else if (value == "host")
				prepared.audio_input_host = true;
			else
				throw std::invalid_argument(
					"--audio-input must be silent or host");
			prepared.audio = true;
		} else if (argument == "--battery") {
			prepared.battery = true;
		} else if (argument == "--video") {
			prepared.video = true;
		} else if (argument == "--video-encoder") {
			prepared.video_encoder = true;
		} else if (valued("--virtio-console-socket=")) {
			set_socket_path(prepared.console_socket,
					"--virtio-console-socket", value);
		} else if (valued("--virtio-serial-socket=")) {
			set_socket_path(prepared.serial_socket,
					"--virtio-serial-socket", value);
		} else if (valued("--gpu-display=")) {
			if (value == "native")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::native;
			else if (value == "offscreen")
				prepared.gpu.display_mode =
					uml::gpu::DisplayMode::offscreen;
			else if (value == "vnc")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::vnc;
			else
				throw std::invalid_argument(
					"--gpu-display must be native, offscreen "
					"or vnc");
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-offscreen-size=")) {
			parse_host_pair(argument, value, 'x',
					prepared.gpu.offscreen.drawableWidth,
					prepared.gpu.offscreen.drawableHeight);
			prepared.gpu.offscreen_option_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-offscreen-mirror=")) {
			/* The mirror Picture in Picture runs while it shows the guest:
			 * 800 pixels wide at 15 frames a second. */
			if (value == "on") {
				prepared.gpu.offscreen.mirrorWidth = 800;
				prepared.gpu.offscreen.mirrorFramesPerSecond = 15;
			} else if (value == "off") {
				prepared.gpu.offscreen.mirrorWidth = 0;
				prepared.gpu.offscreen.mirrorFramesPerSecond = 0;
			} else {
				parse_host_pair(
					argument, value, '@',
					prepared.gpu.offscreen.mirrorWidth,
					prepared.gpu.offscreen.mirrorFramesPerSecond);
			}
			prepared.gpu.offscreen_option_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-vnc-bind=")) {
			prepared.gpu.vnc_option_specified = true;
			if (value.empty())
				throw std::invalid_argument(
					"invalid host option: " +
					std::string(argument));
			prepared.gpu.vnc_bind_address = value;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-vnc-password=")) {
			prepared.gpu.vnc_option_specified = true;
			if (value.empty() || value.size() > 8)
				throw std::invalid_argument(
					"VNC password must contain between 1 and "
					"8 bytes");
			prepared.gpu.vnc_password = value;
			prepared.gpu.enabled = true;
		} else if (parse_host_integer(argument, "--gpu-vnc-port=",
					      prepared.gpu.vnc_port, true)) {
			prepared.gpu.vnc_option_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-auto-resize=")) {
			if (value == "on")
				prepared.gpu.auto_resize_guest = true;
			else if (value == "off")
				prepared.gpu.auto_resize_guest = false;
			else
				throw std::invalid_argument(
					"--gpu-auto-resize must be on or off");
			prepared.gpu.auto_resize_guest_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-budget-app=")) {
			prepared.gpu_budget.application =
				parse_gpu_budget_option(argument, value);
		} else if (valued("--gpu-budget-engine=")) {
			prepared.gpu_budget.engine_rest =
				parse_gpu_budget_option(argument, value);
		} else if (valued("--gpu-budget-headroom=")) {
			prepared.gpu_budget.headroom =
				parse_gpu_budget_option(argument, value);
		} else if (parse_host_integer(
					   argument, "--stats=", prepared.stats_interval_ms)) {
			if (prepared.stats_interval_ms < minimum_stats_interval_ms)
				throw std::invalid_argument(
					"--stats requires an interval of at least " +
					std::to_string(minimum_stats_interval_ms) +
					" milliseconds");
		} else if (valued("--net-forward=")) {
			if (value.empty())
				throw std::invalid_argument(
					"--net-forward requires "
					"[PROTO:][HOSTADDR:]HOSTPORT-"
					"[GUESTADDR:]GUESTPORT");
			prepared.net_forwards.emplace_back(value);
		} else if (valued("--vsock-forward=")) {
			std::string error;
			auto forward = uml::vsock::parseForward(value, error);
			if (!forward)
				throw std::invalid_argument(error);
			prepared.vsock_forwards.push_back(std::move(*forward));
		} else if (
			   parse_host_integer(
				   argument, "--gpu-width=", prepared.gpu.width) ||
			   parse_host_integer(
				   argument, "--gpu-height=", prepared.gpu.height) ||
			   parse_host_integer(
				   argument, "--gpu-dpi=",
				   prepared.gpu.dots_per_inch)) {
			prepared.gpu.enabled = true;
		} else {
			prepared.kernel.push_back(text);
		}
	}
	if (prepared.gpu.display_mode != uml::gpu::DisplayMode::vnc &&
	    prepared.gpu.vnc_option_specified)
		throw std::invalid_argument(
			"--gpu-vnc-* options require --gpu-display=vnc");
	if (prepared.gpu.display_mode != uml::gpu::DisplayMode::offscreen &&
	    prepared.gpu.offscreen_option_specified)
		throw std::invalid_argument(
			"--gpu-offscreen-* options require "
			"--gpu-display=offscreen");
	if (prepared.gpu.display_mode != uml::gpu::DisplayMode::native &&
	    prepared.gpu.auto_resize_guest_specified)
		throw std::invalid_argument(
			"--gpu-auto-resize requires --gpu-display=native");
	if (prepared.gpu.dots_per_inch != 0 &&
	    (prepared.gpu.width > uml::gpu::protocol::maximumEdidDimension ||
	     prepared.gpu.height > uml::gpu::protocol::maximumEdidDimension))
		throw std::invalid_argument(
			"--gpu-dpi requires display dimensions no larger than 4095");
	if (!prepared.console_socket.empty() && !prepared.serial_socket.empty())
		throw std::invalid_argument(
			"--virtio-console-socket and --virtio-serial-socket are "
			"mutually exclusive");
	append_default_kernel_argument(prepared.kernel, "mem=",
		uml::emulator::default_guest_memory_argument(
			prepared.kernel, configuration.default_guest_memory));
	append_default_kernel_argument(prepared.kernel, "ncpus=",
		"ncpus=" + std::to_string(
			uml::emulator::default_application_guest_cpu_count()));
	if (configuration.top_byte_ignore)
		prepared.kernel.emplace_back("--guest-top-byte-ignore");
	return prepared;
}

bool has_virtio_device_argument(const std::vector<std::string> &arguments,
				std::uint32_t device_id)
{
	const std::string suffix = ":" + std::to_string(device_id);
	for (const auto &argument : arguments)
		if (argument.starts_with("virtio_uml.device=") &&
		    argument.ends_with(suffix))
			return true;
	return false;
}

/* Starts the in-process Virtio device that `make` creates and appends the
 * kernel argument that hands it to the guest, unless the command line already
 * names a transport for that device. A refusal or a start failure prints its
 * reason and returns false: `subject` names what asked for the device,
 * `device` what failed to start and `transport` (`device` when null) what the
 * explicit argument would collide with. */
template<typename Make>
[[nodiscard]] bool start_virtio_device(
	std::vector<std::string> &kernel,
	std::unique_ptr<uml::gpu::InProcessVhostUserService> &service,
	std::uint32_t device_id, Make make, const char *subject,
	const char *device, const char *transport = nullptr)
{
	if (has_virtio_device_argument(kernel, device_id)) {
		std::fprintf(stderr,
			"%s cannot be combined with an explicit %s transport "
			"argument\n",
			subject, transport != nullptr ? transport : device);
		return false;
	}
	try {
		service = make();
		kernel.emplace_back(uml::gpu::virtioDeviceArgument(
			service->takeConnectionName(), device_id));
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to start in-process %s: %s\n",
			device, error.what());
		return false;
	}
	return true;
}

/* Whether `argument` asks for a run that only prints information and boots
 * no guest. */
bool informational_option(std::string_view argument)
{
	return argument == "--help" || argument == "--version" ||
		argument == "--showconfig";
}

using VirtioService = std::unique_ptr<uml::gpu::InProcessVhostUserService>;

/* The thread behind --stats: once an interval it reads the running guest's
 * figures through the public calls, as the iOS applications do, and prints
 * the line statistics_report.hpp builds. It exists only when the option does,
 * so a launch without it has no sampler, timer or counter of its own. */
class StatsSampler final {
public:
	using Clock = std::chrono::steady_clock;

	StatsSampler(std::chrono::milliseconds interval, uml_emulator *emulator,
				 uml::gpu::InProcessGpuService *gpu)
		: interval_(interval), emulator_(emulator), gpu_(gpu),
		  thread_([this] { run(); })
	{
	}

	StatsSampler(const StatsSampler &) = delete;
	StatsSampler &operator=(const StatsSampler &) = delete;

	~StatsSampler() { stop(); }

	void stop() noexcept
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		changed_.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

private:
	void run()
	{
		(void)::pthread_setname_np("uml-stats");
		uml::emulator::StatisticsReport report;
		auto due = Clock::now() + interval_;
		for (;;) {
			{
				std::unique_lock lock(mutex_);
				if (changed_.wait_until(
						lock, due, [this] { return stopping_; }))
					return;
			}
			due += interval_;
			/* A sampler that fell behind (a memory read can wait up to
			 * 500 ms) does not catch up with a burst. */
			if (const auto now = Clock::now(); due < now)
				due = now + interval_;
			sample(report);
		}
	}

	void sample(uml::emulator::StatisticsReport &report)
	{
		uml_emulator_statistics guest {.struct_size = sizeof(guest)};
		if (uml_emulator_get_statistics(emulator_, &guest) != UML_EMULATOR_OK) {
			/* No guest runs: before its kernel starts, between two boots. */
			report.guest_ended();
			return;
		}
		uml::emulator::StatisticsSample reading {
			.at = Clock::now(),
			.processors = guest.guest_cpu_count,
			.cpu_time_ns = guest.guest_cpu_time_ns,
			.ram_bytes = guest.guest_ram_bytes,
			.ram_held_bytes = guest.guest_ram_held_bytes,
		};
		std::optional<bool> scanout;
		if (gpu_ != nullptr) {
			if (const auto frames = gpu_->displayStatistics()) {
				reading.presented_frames = frames->presentedFrames;
				reading.offered_frames = frames->guestFrames;
			}
			reading.gpu_memory_refusals = gpu_->memoryRefusals();
			if (const auto memory = gpu_->deviceMemory()) {
				reading.gpu_memory_budget_bytes = memory->budgetBytes;
				reading.gpu_memory_used_bytes = memory->usageBytes;
			}
			/* Holders of 1 MiB or more, quiet or not: the line is for a
			 * person comparing it with what the guest runs. */
			if (const auto memory = gpu_->graphicsMemory(
					8, std::chrono::milliseconds {0}, 1U << 20)) {
				uml::emulator::GraphicsAttribution attribution {
					.context_bytes = memory->contextBytes,
				};
				for (const auto &holder : memory->holders)
					attribution.holders.push_back({
						.pid = holder.pid,
						.bytes = holder.bytes,
						.idle_ms = static_cast<std::uint64_t>(
							std::chrono::duration_cast<
								std::chrono::milliseconds>(holder.idle)
								.count()),
						.name = holder.name,
					});
				reading.graphics_attribution = std::move(attribution);
				reading.guest_reclaims = memory->guestReclaims;
			}
			scanout = gpu_->guestScanoutActive();
		}
		/* A guest whose display is off may have suspended itself, and the
		 * request would end that: the applications do not ask either. */
		std::optional<uml::emulator::StatisticsMemory> memory;
		if (report.memory_due() && (!scanout || *scanout)) {
			uml_emulator_guest_memory figures {.struct_size = sizeof(figures)};
			if (uml_emulator_get_guest_memory(emulator_, &figures) ==
				UML_EMULATOR_OK)
				memory = uml::emulator::StatisticsMemory {
					.total_bytes = figures.total_bytes,
					.available_bytes = figures.available_bytes,
				};
		}
		if (const auto line = report.add(reading, memory))
			std::fprintf(stderr, "%s\n", line->c_str());
	}

	const std::chrono::milliseconds interval_;
	uml_emulator *const emulator_;
	uml::gpu::InProcessGpuService *const gpu_;
	std::mutex mutex_;
	std::condition_variable changed_;
	bool stopping_ {};
	/* Last, so everything above exists when the thread starts. */
	std::thread thread_;
};

/* The Virtio services that belong to one boot. The guest they serve is gone
 * once its kernel returns, so a restarted guest is given fresh ones. */
struct BootServices final {
	VirtioService battery;
	VirtioService video;
	VirtioService video_encoder;
	VirtioService audio;
	/* The guest records silence unless the user asked for the host's
	 * microphone; the switch ends with the boot's audio service. */
	std::shared_ptr<uml::audio::MicrophoneSwitch> microphone;
	VirtioService console;
	/* The vsock device and the loopback listeners that open connections
	 * through it; the listeners stop first. */
	VirtioService vsock;
	std::unique_ptr<uml::vsock::Forwarder> vsock_forwarder;

	void stop() noexcept
	{
		vsock_forwarder.reset();
		if (vsock)
			vsock->stop();
		if (audio)
			audio->stop();
		if (battery)
			battery->stop();
		if (video)
			video->stop();
		if (video_encoder)
			video_encoder->stop();
		if (console)
			console->stop();
	}
};

/* One launch: the machine and the boots of its guest.
 *
 * The machine is what a guest that restarts itself keeps: the emulator
 * handle, the user network with the forwards it was asked for, and the GPU
 * service with the display it owns (the VNC listener and its clients, or the
 * native window). It is created once. A boot is what the restarted guest does
 * not keep: the Virtio services, the guest's connection to the GPU, and the
 * kernel command line that names them. Every boot creates its own, and the
 * boots follow each other in one process until the guest powers off, is
 * stopped, fails, or restarts early too often. */
class Launch final {
public:
	Launch(const uml::app::LaunchConfiguration &configuration,
	       PreparedArguments prepared, bool information_only, char *envp[])
		: configuration_(configuration), prepared_(std::move(prepared)),
		  information_only_(information_only)
	{
		for (auto entry = envp; entry != nullptr && *entry != nullptr; ++entry)
			environment_.push_back(*entry);
		environment_.push_back(nullptr);
	}

	/* Creates the machine. False, with the reason on stderr, when that
	 * fails. */
	[[nodiscard]] bool create()
	{
		if (user_network_enabled()) {
			try {
				network_ = std::make_unique<AppNetwork>();
				network_callbacks_ = network_->callbacks();
			} catch (const std::exception &error) {
				std::fprintf(stderr, "Unable to create user network: %s\n",
					error.what());
				return false;
			}
			for (const auto &specification : prepared_.net_forwards) {
				if (network_->forward(specification) == 0)
					continue;
				std::fprintf(stderr,
					"Unable to accept --net-forward=%s\n",
					specification.c_str());
				return false;
			}
		} else if (!prepared_.net_forwards.empty()) {
			std::fputs("--net-forward requires the user network\n", stderr);
			return false;
		}

		const uml_emulator_config config{
			.abi_version = UML_EMULATOR_ABI_VERSION,
			.struct_size = sizeof(uml_emulator_config),
			.rootfs_path = configuration_.rootfs_path,
			.kernel_image = uml_elf_start,
			.kernel_image_size = static_cast<std::size_t>(
				uml_elf_end - uml_elf_start),
			.network = network_ ? &network_callbacks_ : nullptr,
		};
		uml_emulator *created = nullptr;
		const int create_result = uml_emulator_create(&config, &created);
		if (create_result != UML_EMULATOR_OK) {
			std::fprintf(stderr, "Unable to create UML emulator: %d\n",
				create_result);
			return false;
		}
		emulator_.reset(created);
		/* GPU memory is charged to the process like guest RAM: the GPU's
		 * budget is what the process limit leaves after the guest, the
		 * gadget cache and the launch's own allowances, at least 384 MiB and
		 * at most half of the guest's allocation. */
		const auto plan = uml::emulator::plan_gpu_budget(
			prepared_.kernel, prepared_.gpu_budget);
		device_memory_limit_ = plan.budget.bytes;
		if (prepared_.gpu.enabled && !information_only_) {
			if (plan.budget.short_of_floor)
				std::fprintf(stderr, "uml-darwin: %s\n",
					uml::emulator::describe_gpu_budget_shortfall(
						plan)
						.c_str());
			else if (plan.process_limit != 0)
				std::fprintf(stderr,
					"uml-darwin: GPU budget %llu MiB of a "
					"%llu MiB process limit\n",
					static_cast<unsigned long long>(
						plan.budget.bytes >> 20),
					static_cast<unsigned long long>(
						plan.process_limit >> 20));
		}
		if (prepared_.gpu.enabled && !information_only_) {
			if (has_virtio_device_argument(
				prepared_.kernel, uml::gpu::protocol::virtioGpuDeviceId)) {
				std::fputs("--gpu cannot be combined with an explicit "
					   "Virtio-GPU transport argument\n",
					   stderr);
				return false;
			}
			try {
				gpu_service_ =
					std::make_unique<uml::gpu::InProcessGpuService>(
						uml::gpu::InProcessGpuOptions{
							.displayMode = prepared_.gpu.display_mode,
							.vncBindAddress =
								prepared_.gpu.vnc_bind_address,
							.vncPort = prepared_.gpu.vnc_port,
							.vncPassword =
								prepared_.gpu.vnc_password,
							.width = prepared_.gpu.width,
							.height = prepared_.gpu.height,
							.dotsPerInch =
								prepared_.gpu.dots_per_inch,
							.pointerMode = configuration_.input_mode !=
									uml::app::DisplayInput::mouse ?
								uml::input::vnc::PointerMode::touchscreen :
								uml::input::vnc::PointerMode::mouse,
							.autoResizeGuest =
								prepared_.gpu.auto_resize_guest,
							.offscreen = prepared_.gpu.offscreen,
							.sharedMemoryRegions =
								shared_memory_regions_,
							.deviceMemoryLimit =
								device_memory_limit_,
						});
			} catch (const std::exception &error) {
				std::fprintf(stderr,
					"Unable to start in-process Virtio-GPU: %s\n",
					error.what());
				return false;
			}
		}
		if (prepared_.stats_interval_ms != 0 && !information_only_)
			stats_ = std::make_unique<StatsSampler>(
				std::chrono::milliseconds(prepared_.stats_interval_ms),
				emulator_.get(), gpu_service_.get());
		return true;
	}

	/* Whether the guest's window, if it has one, wants the guest to stop. */
	[[nodiscard]] bool native_display() const noexcept
	{
		return prepared_.gpu.enabled &&
			prepared_.gpu.display_mode == uml::gpu::DisplayMode::native;
	}

	/* Ends the run in progress, and every boot after it. May be called from
	 * any thread. A stop request made while no run is active is not inherited
	 * by the next one, so the loop also keeps the request itself. */
	void request_stop() noexcept
	{
		stop_requested_.store(true, std::memory_order_release);
		uml_emulator_request_stop(emulator_.get());
	}

	/* Boots the guest, and boots it again each time it restarts itself,
	 * until it powers off, is stopped, fails to run, or the loop guard ends
	 * the loop. Returns the exit status of the launch. */
	[[nodiscard]] int run()
	{
		uml::emulator::RestartLoopGuard guard;
		std::vector<std::string> base_kernel = prepared_.kernel;
		unsigned restarts = 0;
		for (;;) {
			std::vector<std::string> kernel = base_kernel;
			if (!start_boot(kernel, restarts != 0))
				return 1;
			std::vector<const char *> arguments;
			arguments.reserve(kernel.size());
			for (const auto &argument : kernel)
				arguments.push_back(argument.c_str());
			guard.boot_started(uml::emulator::RestartLoopGuard::Clock::now());
			/* A stop that came between two boots was dropped by the handle. */
			if (stop_requested_.load(std::memory_order_acquire))
				return 0;
			int status = enter(arguments);
			/* The library refuses to take a restarted guest back while a
			 * kernel thread of the old one is still ending, and changes
			 * nothing when it does. */
			for (unsigned attempt = 1;
			     restarts != 0 && status == -UML_EMULATOR_RUNTIME_ERROR &&
			     attempt <= uml::emulator::restart_run_retries &&
			     !stop_requested_.load(std::memory_order_acquire);
			     ++attempt) {
				std::fprintf(stderr,
					"The restarted guest is not handed back yet; trying "
					"again (%u of %u)\n", attempt,
					uml::emulator::restart_run_retries);
				std::this_thread::sleep_for(
					uml::emulator::restart_run_retry_delay);
				status = enter(arguments);
			}
			uml_emulator_run_end end{.struct_size = sizeof(end)};
			const bool known =
				uml_emulator_get_run_end(emulator_.get(), &end) ==
				UML_EMULATOR_OK;
			if (restarts != 0 && status < 0) {
				std::fprintf(stderr,
					"Unable to boot the restarted guest: %d\n", status);
				return 1;
			}
			if (!known || end.kind != UML_EMULATOR_RUN_END_RESTART)
				return status;
			/* The guest restarted as the user quit: the quit wins, and is
			 * a clean stop whatever status the restart carried. */
			if (stop_requested_.load(std::memory_order_acquire))
				return 0;
			/* The guest is gone: its services stop before the library hands
			 * its RAM back to the process, and its connections end with its
			 * network link. */
			services_.stop();
			if (network_)
				network_->reset_link();
			if (!guard.may_restart(
				    uml::emulator::RestartLoopGuard::Clock::now())) {
				std::fprintf(stderr,
					"The guest restarted %u times in a row within %lld s "
					"of booting; not booting it again (status %d)\n",
					guard.early_restarts(),
					static_cast<long long>(
						uml::emulator::early_restart_window.count()),
					uml::emulator::early_restart_exit_status);
				return uml::emulator::early_restart_exit_status;
			}
			++restarts;
			end.restart_command[sizeof(end.restart_command) - 1U] = '\0';
			std::fprintf(stderr,
				"guest restarting (command '%s', restart %u)\n",
				uml::emulator::printable_restart_command(
					end.restart_command).c_str(),
				restarts);
			if (configuration_.restart_command_line &&
			    !take_restart_command_line(base_kernel))
				return 1;
		}
	}

	/* Ends the launch: the network first, then what the boot left. */
	void finish() noexcept
	{
		stats_.reset();
		if (network_)
			network_->shutdown();
		services_.stop();
		if (gpu_service_)
			gpu_service_->stop();
		if (!configuration_.run_scratch.empty()) {
			std::error_code ignored;
			std::filesystem::remove_all(configuration_.run_scratch, ignored);
		}
	}

private:
	[[nodiscard]] int enter(const std::vector<const char *> &arguments)
	{
		return uml_emulator_run(
			emulator_.get(), static_cast<int>(arguments.size()),
			arguments.data(), environment_.data());
	}

	/* The kernel arguments of the boot after a restart: what the launcher's
	 * restart command line says, which may differ from the first boot's (an
	 * Android guest picks its ramdisk again). The host devices it asks for
	 * must be the first boot's, which the machine was created for. */
	[[nodiscard]] bool take_restart_command_line(
		std::vector<std::string> &base_kernel)
	{
		try {
			auto next = prepare_host_arguments(
				configuration_.restart_command_line(), configuration_);
			if (!prepared_.same_host_devices(next) ||
			    !prepared_.same_machine(next)) {
				std::fputs("The restart command line asks for another "
					   "machine than the first boot's\n", stderr);
				return false;
			}
			base_kernel = std::move(next.kernel);
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to prepare the restarted guest: %s\n",
				error.what());
			return false;
		}
		return true;
	}

	/* Starts the boot's services, in the order the guest's Virtio devices are
	 * numbered by, and names them on `kernel`. `restart` says an earlier
	 * guest of this launch exists, whose GPU connection the new one
	 * replaces. */
	[[nodiscard]] bool start_boot(std::vector<std::string> &kernel,
				      bool restart)
	{
		services_ = {};
		if (gpu_service_) {
			try {
				if (restart)
					gpu_service_->startNextConnection();
				kernel.emplace_back(uml::gpu::virtioDeviceArgument(
					gpu_service_->takeConnectionName(),
					uml::gpu::protocol::virtioGpuDeviceId));
				kernel.emplace_back("uml_vnc_input." +
					gpu_service_->takeInputConnectionName());
			} catch (const std::exception &error) {
				std::fprintf(stderr,
					restart ? "Unable to restart in-process Virtio-GPU: %s\n" :
						  "Unable to start in-process Virtio-GPU: %s\n",
					error.what());
				return false;
			}
		}
		/* Android's vendor policy labels the battery's sysfs path by its
		 * position among the Virtio devices
		 * (android/lineage-24/overlay/sepolicy/vendor/genfs_contexts).
		 * Straight after the GPU it is always virtio-uml.1/virtio1, whatever
		 * optional devices follow. */
		if (prepared_.battery && !information_only_ &&
		    !start_virtio_device(
			    kernel, services_.battery,
			    uml::battery::protocol::deviceId,
			    [] { return uml::battery::makeInProcessService(); },
			    "--battery", "Virtio battery"))
			return false;
		/* The guest's host-visible shared-memory region belongs to the GPU
		 * backend, and the video decoder fills capture buffers that live in
		 * it, so both backends share one index of the ranges published
		 * there. */
		if (prepared_.video && !information_only_ &&
		    !start_virtio_device(
			    kernel, services_.video, uml::video::deviceId,
			    [&] {
				    return uml::video::makeInProcessService(
					    gpu_service_ ? shared_memory_regions_ : nullptr);
			    },
			    "--video", "Virtio video"))
			return false;
		if (prepared_.video_encoder && !information_only_ &&
		    !start_virtio_device(
			    kernel, services_.video_encoder,
			    uml::video::encoderDeviceId,
			    [&] {
				    return uml::video::makeInProcessEncoderService(
					    gpu_service_ ? shared_memory_regions_ : nullptr);
			    },
			    "--video-encoder", "Virtio video encoder"))
			return false;
		/* Creating the switch touches nothing; only turning it on below
		 * can make the system ask for permission. */
		if (prepared_.audio_input_host)
			services_.microphone =
				std::make_shared<uml::audio::MicrophoneSwitch>(
					uml::platform::makeAppleMicrophoneAuthorization());
		if (prepared_.audio && !information_only_ &&
		    !start_virtio_device(
			    kernel, services_.audio,
			    uml::audio::protocol::deviceId,
			    [&] {
				    return uml::audio::makeInProcessService(
					    {.output = prepared_.audio_output,
					     .microphone = services_.microphone});
			    },
			    "--audio", "Virtio Sound"))
			return false;
		if (services_.microphone && services_.audio) {
			using State = uml::audio::MicrophoneSwitch::State;
			const auto report = [](State state) {
				std::fprintf(stderr, "Microphone: %s\n",
					     uml::audio::describe(state));
			};
			report(services_.microphone->enable(report));
		}
		const bool named_serial = !prepared_.serial_socket.empty();
		if ((named_serial || !prepared_.console_socket.empty()) &&
		    !information_only_ &&
		    !start_virtio_device(
			    kernel, services_.console,
			    uml::console::protocol::deviceId,
			    [&] {
				    return named_serial ?
					    uml::console::makeInProcessService(
						    prepared_.serial_socket,
						    std::string(uml::console::protocol::
								hostSerialPortName)) :
					    uml::console::makeInProcessService(
						    prepared_.console_socket);
			    },
			    "a host Virtio console or serial socket",
			    named_serial ? "Virtio serial" : "Virtio console",
			    "Virtio console"))
			return false;
		/* Last, so that no other device's index moves. */
		if (!prepared_.vsock_forwards.empty() && !information_only_ &&
		    !start_vsock(kernel))
			return false;
		return true;
	}

	/* Starts the Virtio vsock device and the listeners that reach it, and
	 * says which port each one listens on. */
	[[nodiscard]] bool start_vsock(std::vector<std::string> &kernel)
	{
		std::shared_ptr<uml::vsock::Endpoint> endpoint;
		if (!start_virtio_device(
			    kernel, services_.vsock, uml::vsock::protocol::deviceId,
			    [&] {
				    auto started = uml::vsock::makeInProcessService();
				    endpoint = std::move(started.endpoint);
				    return std::move(started.service);
			    },
			    "--vsock-forward", "Virtio vsock"))
			return false;
		try {
			services_.vsock_forwarder =
				std::make_unique<uml::vsock::Forwarder>(
					endpoint, prepared_.vsock_forwards);
		} catch (const std::exception &error) {
			std::fprintf(stderr, "Unable to start the vsock forward: %s\n",
				error.what());
			return false;
		}
		for (std::size_t index = 0; index < prepared_.vsock_forwards.size();
		     ++index) {
			const auto &forward = prepared_.vsock_forwards[index];
			std::fprintf(stderr,
				"vsock forward %s:%u -> guest port %u\n",
				forward.hostAddress.c_str(),
				static_cast<unsigned>(services_.vsock_forwarder->port(index)),
				static_cast<unsigned>(forward.guestPort));
		}
		return true;
	}

	const uml::app::LaunchConfiguration &configuration_;
	PreparedArguments prepared_;
	const bool information_only_;
	std::vector<const char *> environment_;
	std::unique_ptr<AppNetwork> network_;
	uml_emulator_network_callbacks network_callbacks_{};
	std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> emulator_{
		nullptr, uml_emulator_destroy};
	std::shared_ptr<uml::virtio::SharedMemoryRegions> shared_memory_regions_{
		std::make_shared<uml::virtio::SharedMemoryRegions>()};
	std::unique_ptr<uml::gpu::InProcessGpuService> gpu_service_;
	/* After the handle and the GPU service it reads, so it ends before them. */
	std::unique_ptr<StatsSampler> stats_;
	BootServices services_;
	std::uint64_t device_memory_limit_{};
	std::atomic<bool> stop_requested_{};
};

} // namespace

bool uml::app::has_argument(
	int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool uml::app::informational(int argc, char *const argv[])
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && informational_option(argv[index]))
			return true;
	return false;
}

int uml::app::runEmbeddedKernel(
	const std::vector<std::string> &command_line, char *envp[],
	const LaunchConfiguration &configuration)
{
	install_host_faults();
	ignore_broken_pipes();
	::uml::device::install_socket_path_signal_cleanup();
	PreparedArguments prepared;
	try {
		prepared = prepare_host_arguments(command_line, configuration);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure host devices: %s\n",
			     error.what());
		return 1;
	}
	bool information_only = false;
	for (std::size_t index = 1; index < command_line.size(); ++index)
		information_only |= informational_option(command_line[index]);
	/* Before anything in the process can create a Metal device. */
	if (prepared.gpu.enabled && !information_only) {
		bound_metal_shader_cache();
		place_mesa_shader_cache();
	}
	Launch launch{configuration, std::move(prepared), information_only, envp};
	if (!launch.create())
		return 1;
	/* A native display runs AppKit on the main thread and the guest on a
	 * worker; closing its window stops the guest. The boots all run on the
	 * worker, so the window outlives a guest that restarts. */
	const int result = launch.native_display() ?
		uml::gpu::runNativeDisplayApplication(
			[&] { return launch.run(); },
			[&] { launch.request_stop(); }) :
		launch.run();
	launch.finish();
	/* The loader hands the kernel's signal state back to the host before
	 * uml_elf_run returns, so no Linux handler is left behind. The launcher
	 * still leaves through _Exit, which runs no static destructor or atexit
	 * handler; code that must write at exit, such as the guest-code
	 * sampler, does so before this point.
	 */
	std::_Exit(result);
}
