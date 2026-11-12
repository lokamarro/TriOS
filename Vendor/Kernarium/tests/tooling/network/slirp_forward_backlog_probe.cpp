/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Several host clients may connect to one forward at once: an adb client and
 * its tools, a browser, a parallel test runner. SLiRP accepts one connection
 * per event-loop pass, so the host kernel queues the others on the forward's
 * listening socket, and a queue shorter than the burst refuses or resets the
 * overflow before SLiRP sees it. Connect a burst at once and require every
 * client to reach the guest as its own SYN, and none to fail.
 */
#include "provider.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <span>
#include <vector>

namespace {

/* Well under the host's SOMAXCONN (128 on macOS), far over a backlog of 1. */
constexpr std::size_t clients = 32;
constexpr std::uint16_t guest_port = 7;
constexpr std::array<std::uint8_t, 4> guest_ip{10, 0, 2, 15};
constexpr std::array<std::uint8_t, 4> gateway_ip{10, 0, 2, 2};

std::uint16_t read16(std::span<const std::byte> bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>(
		(std::to_integer<unsigned int>(bytes[offset]) << 8) |
		std::to_integer<unsigned int>(bytes[offset + 1]));
}

void write16(std::span<std::byte> bytes, std::size_t offset,
	     std::uint16_t value)
{
	bytes[offset] = std::byte(value >> 8);
	bytes[offset + 1] = std::byte(value & 0xff);
}

/* The guest's ARP request for the gateway: SLiRP learns the guest's MAC from
 * it, and sends a forwarded connection's SYN only to a known MAC. */
std::array<std::byte, 64> arp_request(std::span<const unsigned char, 6> mac)
{
	std::array<std::byte, 64> frame{};
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = std::byte{0xff};
		frame[6 + index] = std::byte(mac[index]);
		frame[22 + index] = std::byte(mac[index]);
	}
	write16(frame, 12, 0x0806);
	write16(frame, 14, 1);
	write16(frame, 16, 0x0800);
	frame[18] = std::byte{6};
	frame[19] = std::byte{4};
	write16(frame, 20, 1);
	for (std::size_t index = 0; index < 4; ++index) {
		frame[28 + index] = std::byte(guest_ip[index]);
		frame[38 + index] = std::byte(gateway_ip[index]);
	}
	return frame;
}

bool is_arp_reply(std::span<const std::byte> frame)
{
	return frame.size() >= 42 && read16(frame, 12) == 0x0806 &&
		read16(frame, 20) == 2;
}

/* The source port of a SYN (without ACK) SLiRP sends to the guest's port. */
int syn_source_port(std::span<const std::byte> frame)
{
	if (frame.size() < 34 || read16(frame, 12) != 0x0800 ||
	    std::to_integer<unsigned int>(frame[23]) != IPPROTO_TCP)
		return -1;
	const std::size_t header =
		(std::to_integer<std::size_t>(frame[14]) & 0x0f) * 4;
	const std::size_t tcp = 14 + header;
	if (frame.size() < tcp + 14 || read16(frame, tcp + 2) != guest_port)
		return -1;
	const auto flags = std::to_integer<unsigned int>(frame[tcp + 13]);
	if ((flags & 0x12) != 0x02)
		return -1;
	return read16(frame, tcp);
}

std::uint16_t unused_loopback_port()
{
	const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0)
		return 0;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	std::uint16_t port = 0;
	if (bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
		 sizeof(address)) == 0 &&
	    getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
			&length) == 0)
		port = ntohs(address.sin_port);
	close(descriptor);
	return port;
}

bool wait_readable(int descriptor, int timeout_ms)
{
	pollfd entry{descriptor, POLLIN, 0};
	return poll(&entry, 1, timeout_ms) == 1 && (entry.revents & POLLIN) != 0;
}

/* A client failed when its connect was refused or the connection was reset
 * or closed; one SLiRP holds for a silent guest stays open and quiet. */
bool client_failed(int descriptor)
{
	int error = 0;
	socklen_t length = sizeof(error);
	if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &length) != 0 ||
	    error != 0)
		return true;
	pollfd entry{descriptor, POLLIN, 0};
	if (poll(&entry, 1, 0) != 1)
		return false;
	std::byte byte{};
	const auto received = recv(descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	return received == 0 || (received < 0 && errno != EAGAIN);
}

} // namespace

int main(void)
{
	uml::network::SlirpProvider provider;
	uml::network::HostForward forward{};
	forward.host_port = unused_loopback_port();
	forward.guest_port = guest_port;
	if (forward.host_port == 0 || provider.add_forward(forward) != 0) {
		fprintf(stderr, "could not register a loopback forward\n");
		return 1;
	}
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) != 0) {
		fprintf(stderr, "the provider did not open\n");
		return 1;
	}

	std::array<std::byte, uml::network::frame_size> frame{};
	const auto request = arp_request(mac);
	bool resolved = false;
	if (provider.write(request) == 0) {
		for (int attempt = 0; !resolved && attempt < 20; ++attempt) {
			if (!wait_readable(provider.irq_fd(), 100))
				continue;
			const int length = provider.read(frame);
			resolved = length > 0 &&
				is_arp_reply(std::span{frame}.first(
					static_cast<std::size_t>(length)));
		}
	}
	if (!resolved) {
		fprintf(stderr, "SLiRP did not answer the guest's ARP request\n");
		return 1;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(forward.host_port);
	std::vector<int> descriptors;
	for (std::size_t index = 0; index < clients; ++index) {
		const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor < 0 ||
		    fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0) {
			fprintf(stderr, "could not create client %zu\n", index);
			return 1;
		}
		descriptors.push_back(descriptor);
		if (connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
			    sizeof(address)) != 0 &&
		    errno != EINPROGRESS) {
			fprintf(stderr, "client %zu could not connect: %s\n", index,
				strerror(errno));
		}
	}

	/* A short window: a kernel that drops rather than resets an overflow
	 * lets the client retry after a second, which would hide the queue. */
	std::set<int> reached;
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds{900};
	while (reached.size() < clients &&
	       std::chrono::steady_clock::now() < deadline) {
		if (!wait_readable(provider.irq_fd(), 50))
			continue;
		for (;;) {
			const int length = provider.read(frame);
			if (length <= 0)
				break;
			const int port = syn_source_port(std::span{frame}.first(
				static_cast<std::size_t>(length)));
			if (port >= 0)
				reached.insert(port);
		}
	}
	std::size_t failed = 0;
	for (const int descriptor : descriptors)
		failed += client_failed(descriptor) ? 1 : 0;
	for (const int descriptor : descriptors)
		close(descriptor);
	provider.stop();

	if (reached.size() != clients || failed != 0) {
		fprintf(stderr,
			"%zu concurrent connections to one forward: %zu reached "
			"the guest, %zu were refused or reset\n",
			clients, reached.size(), failed);
		return 1;
	}
	printf("PASS: %zu concurrent host connections to one forward each "
	       "reached the guest and none was refused or reset\n",
	       clients);
	return 0;
}
