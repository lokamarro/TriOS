/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libslirp writes the host sockets it makes or accepts with plain send(),
 * which raises SIGPIPE when the peer has reset the connection. A guest that
 * restarts leaves its connections for the host to wind down, and a host
 * client that resets one meanwhile (an adb server dropping its transport) must
 * cost the process nothing, whatever disposition the code that runs the guest
 * left SIGPIPE in: the provider asks every socket libslirp registers for EPIPE
 * instead of the signal.
 *
 * A host client connects to a forward and the probe, as the guest, accepts
 * the connection. The client then resets it while the provider is held
 * between two passes with the guest's data waiting, so the next pass writes
 * that data into a socket whose peer is gone.
 */
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t guest_port = 7;
std::atomic<int> sigpipes;

void count_sigpipe(int) { ++sigpipes; }

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

/* Frames SLiRP has for the guest, as the harness parses them. */
std::vector<std::vector<std::uint8_t>> drain(
	uml::network::SlirpProvider &provider)
{
	std::vector<std::vector<std::uint8_t>> frames;
	std::array<std::byte, uml::network::frame_size> buffer{};
	for (;;) {
		const int length = provider.read(buffer);
		if (length <= 0)
			return frames;
		const auto *bytes =
			reinterpret_cast<const std::uint8_t *>(buffer.data());
		frames.emplace_back(bytes, bytes + length);
	}
}

void send_to_slirp(uml::network::SlirpProvider &provider,
		   const std::vector<std::uint8_t> &frame)
{
	provider.write(std::as_bytes(std::span{frame}));
}

} // namespace

int main(void)
{
	struct sigaction action{};
	action.sa_handler = count_sigpipe;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPIPE, &action, nullptr) != 0)
		return 1;

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

	/* The guest asks for the gateway, so SLiRP knows where to send a SYN. */
	std::vector<std::uint8_t> request(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		request[index] = 0xff;
		request[6 + index] = slirp_harness::guest_mac[index];
		request[22 + index] = slirp_harness::guest_mac[index];
	}
	slirp_harness::put16(request, 12, 0x0806);
	slirp_harness::put16(request, 14, 1);
	slirp_harness::put16(request, 16, 0x0800);
	request[18] = 6;
	request[19] = 4;
	slirp_harness::put16(request, 20, 1);
	for (std::size_t index = 0; index < 4; ++index) {
		request[28 + index] = slirp_harness::guest_ip[index];
		request[38 + index] = slirp_harness::gateway_ip[index];
	}
	send_to_slirp(provider, request);
	bool resolved = false;
	for (int attempt = 0; !resolved && attempt < 20; ++attempt) {
		if (!wait_readable(provider.irq_fd(), 100))
			continue;
		for (const auto &frame : drain(provider))
			resolved |= frame.size() >= 42 &&
				slirp_harness::get16(frame, 12) == 0x0806;
	}
	if (!resolved) {
		fprintf(stderr, "SLiRP did not answer the guest's ARP request\n");
		return 1;
	}

	const int client = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(forward.host_port);
	if (client < 0 ||
	    connect(client, reinterpret_cast<const sockaddr *>(&address),
		    sizeof(address)) != 0) {
		fprintf(stderr, "the host client could not connect\n");
		return 1;
	}

	std::optional<slirp_harness::Segment> syn;
	for (int attempt = 0; !syn && attempt < 30; ++attempt) {
		if (!wait_readable(provider.irq_fd(), 100))
			continue;
		for (const auto &frame : drain(provider)) {
			const auto segment = slirp_harness::parse_tcp(frame);
			if (segment && (segment->flags & slirp_harness::tcp_syn) != 0 &&
			    segment->destination_port == guest_port)
				syn = segment;
		}
	}
	if (!syn) {
		fprintf(stderr, "the connection did not reach the guest\n");
		return 1;
	}
	const std::uint16_t client_port = syn->source_port;
	const std::uint32_t guest_seq = 5000;
	send_to_slirp(provider, slirp_harness::tcp_frame(
		slirp_harness::guest_ip, slirp_harness::gateway_ip, guest_port,
		client_port, guest_seq, syn->seq + 1,
		slirp_harness::tcp_syn | slirp_harness::tcp_ack, 65535));
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	(void)drain(provider);

	/* Hold the provider between passes, give it the guest's data, reset the
	 * connection from the host side, and let it run. */
	provider.pause_transmit_for_testing();
	if (!provider.wait_for_transmit_pause_for_testing()) {
		fprintf(stderr, "the provider did not pause\n");
		return 1;
	}
	for (std::uint32_t segment = 0; segment != 4; ++segment)
		send_to_slirp(provider, slirp_harness::tcp_frame(
			slirp_harness::guest_ip, slirp_harness::gateway_ip,
			guest_port, client_port, guest_seq + 1 + segment * 400,
			syn->seq + 1,
			slirp_harness::tcp_psh | slirp_harness::tcp_ack, 65535,
			400));
	linger reset{1, 0};
	(void)setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
	close(client);
	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	provider.resume_transmit_for_testing();
	for (int pass = 0; pass != 20; ++pass) {
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		(void)drain(provider);
	}
	provider.stop();

	if (sigpipes.load() != 0) {
		fprintf(stderr,
			"%d SIGPIPE raised by writes to a socket the peer reset\n",
			sigpipes.load());
		return 1;
	}
	printf("PASS: a host client that reset its connection while the guest's "
	       "data was waiting raised no SIGPIPE\n");
	return 0;
}
