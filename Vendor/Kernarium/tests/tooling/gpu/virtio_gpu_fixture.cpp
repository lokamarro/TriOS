// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_vhost_user_server.hpp"
#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace {

std::atomic_bool stop_requested;

void stop(int)
{
	stop_requested.store(true, std::memory_order_release);
}

template<typename Integer>
Integer parse_integer(std::string_view value, std::string_view option)
{
	Integer result{};
	const auto [end, error] =
		std::from_chars(value.data(), value.data() + value.size(), result);
	if (error != std::errc{} || end != value.data() + value.size())
		throw std::invalid_argument(
			"invalid value for " + std::string(option));
	return result;
}

void usage(const char *executable)
{
	std::cout
		<< "Usage: " << executable << " [options]\n"
		<< "  --vnc-bind ADDRESS    VNC IPv4 bind address (default: 127.0.0.1)\n"
		<< "  --vnc-port PORT       VNC port (0 selects a dynamic port; default: 5900)\n"
		<< "  --vnc-password PASS   require a VNC password (1-8 bytes)\n"
		<< "  --width PIXELS        scanout width (default: 1280)\n"
		<< "  --height PIXELS       scanout height (default: 720)\n"
		<< "  --self-test-pattern   show deterministic VNC test pixels\n";
}

} // namespace

int main(int argument_count, char **arguments)
{
	try {
		std::string vnc_bind_address = "127.0.0.1";
		std::uint16_t vnc_port = 5900;
		std::string vnc_password;
		std::uint32_t width = 1280;
		std::uint32_t height = 720;
		bool self_test_pattern = false;

		for (int index = 1; index < argument_count; ++index) {
			const std::string_view argument(arguments[index]);
			const auto value = [&](std::string_view option) {
				if (++index >= argument_count)
					throw std::invalid_argument(
						std::string(option) + " requires a value");
				return std::string_view(arguments[index]);
			};
			if (argument == "--vnc-bind")
				vnc_bind_address = value(argument);
			else if (argument == "--vnc-port")
				vnc_port = parse_integer<std::uint16_t>(
					value(argument), argument);
			else if (argument == "--vnc-password") {
				vnc_password = value(argument);
				if (vnc_password.empty() || vnc_password.size() > 8)
					throw std::invalid_argument(
						"VNC password must contain between 1 and 8 bytes");
			} else if (argument == "--width")
				width = parse_integer<std::uint32_t>(value(argument), argument);
			else if (argument == "--height")
				height = parse_integer<std::uint32_t>(value(argument), argument);
			else if (argument == "--self-test-pattern")
				self_test_pattern = true;
			else if (argument == "--help" || argument == "-h") {
				usage(arguments[0]);
				return 0;
			} else {
				throw std::invalid_argument(
					"unknown option: " + std::string(argument));
			}
		}
		std::signal(SIGINT, stop);
		std::signal(SIGTERM, stop);
		std::signal(SIGHUP, stop);

		const auto server = uml::gpu::makeGpuVhostUserServer(
			std::move(vnc_bind_address), vnc_port, std::move(vnc_password),
			width, height, self_test_pattern);
		/* The server serves one end of a socketpair. Nothing speaks on the
		 * other end, which stays open, so the VNC display is served until a
		 * signal stops the server. */
		std::array<int, 2> descriptors{-1, -1};
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
			throw std::runtime_error(
				"vhost-user socketpair failed: " +
				std::string(std::strerror(errno)));
		const uml::gpu::UniqueDescriptor peer{descriptors[0]};
		server->runConnected(descriptors[1], stop_requested);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "uml-virtio-gpu: " << error.what() << '\n';
		return 1;
	}
}
