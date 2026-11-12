/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest connect that the host stack refuses at once must fail the way it
 * does on the network SLiRP models. SLiRP answers such a connect with ICMP
 * net-unreachable, which is right for a destination the host cannot route to,
 * but not for port 0: Darwin refuses it with EADDRNOTAVAIL before it sends
 * anything, where a stack that sent the SYN hears nothing from an unreachable
 * host and leaves the guest connect pending until its own timeout. The SYN
 * must go unanswered, however often the guest retransmits it, and must not
 * leave a host socket behind. The gateway is the host itself, though, which
 * would refuse port 0 with a reset, as Linux does for loopback. Every other
 * synchronous failure keeps its ICMP error, and a refused port is still
 * refused with a reset.
 */
#include "slirp_tcp_harness.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

namespace {

using namespace slirp_harness;

constexpr std::uint16_t guest_port = 40000;
constexpr std::uint32_t guest_iss = 7000;
constexpr std::uint8_t icmp_destination_unreachable = 3;
constexpr std::uint8_t icmp_net_unreachable = 0;

int failures;

/* What SLiRP sent the guest after a SYN. */
struct Reply {
	std::size_t frames{};
	std::optional<Segment> tcp;
	bool icmp{};
	std::uint8_t icmp_type{};
	std::uint8_t icmp_code{};
};

Reply collect(Network &network)
{
	Reply reply;
	while (!network.frames().empty()) {
		const auto frame = std::move(network.frames().front());
		network.frames().pop_front();
		reply.frames++;
		if (auto segment = parse_tcp(frame)) {
			reply.tcp = segment;
			continue;
		}
		if (frame.size() >= 14 + 20 + 8 && get16(frame, 12) == 0x0800 &&
		    frame[23] == IPPROTO_ICMP) {
			const std::size_t icmp = 14 + (frame[14] & 0x0f) * 4u;
			reply.icmp = true;
			reply.icmp_type = frame[icmp];
			reply.icmp_code = frame[icmp + 1];
		}
	}
	return reply;
}

/* Send one guest SYN, then give SLiRP a second of its own clock to answer. */
Reply connect_attempt(Network &network,
		      const std::array<std::uint8_t, 4> &destination,
		      std::uint16_t destination_port, std::uint16_t source_port,
		      int passes = 50)
{
	network.input(tcp_frame(guest_ip, destination, source_port,
				destination_port, guest_iss, 0, tcp_syn, 65535));
	Reply reply = collect(network);
	for (int pass = 0; pass < passes; ++pass) {
		network.pump(20, 5);
		const Reply later = collect(network);
		reply.frames += later.frames;
		if (later.tcp)
			reply.tcp = later.tcp;
		if (later.icmp) {
			reply.icmp = true;
			reply.icmp_type = later.icmp_type;
			reply.icmp_code = later.icmp_code;
		}
	}
	return reply;
}

int open_descriptors()
{
	int count = 0;
	for (int descriptor = 3; descriptor < 1024; ++descriptor)
		if (fcntl(descriptor, F_GETFD) != -1)
			count++;
	return count;
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

void expect_unanswered(const char *label, const Reply &reply)
{
	if (reply.frames == 0)
		return;
	fprintf(stderr,
		"%s: SLiRP answered with %zu frame(s): tcp=%d icmp=%d "
		"(type %u code %u)\n",
		label, reply.frames, reply.tcp ? 1 : 0, reply.icmp ? 1 : 0,
		static_cast<unsigned int>(reply.icmp_type),
		static_cast<unsigned int>(reply.icmp_code));
	failures++;
}

} // namespace

int main(void)
{
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fprintf(stderr, "SLiRP did not start or answer ARP\n");
		return 1;
	}

	/* RFC 5737 TEST-NET-1: nothing answers there on a real network. */
	const std::array<std::uint8_t, 4> unreachable{192, 0, 2, 0};

	/* Port 0: no reply, for the SYN and for a retransmission of it, and
	 * no host socket left open by any attempt. */
	const int before = open_descriptors();
	expect_unanswered("port 0 SYN",
			  connect_attempt(network, unreachable, 0, guest_port));
	expect_unanswered("port 0 retransmitted SYN",
			  connect_attempt(network, unreachable, 0, guest_port));
	for (std::uint16_t offset = 1; offset <= 16 && failures == 0; ++offset)
		expect_unanswered("port 0 SYN from another port",
				  connect_attempt(network, unreachable, 0,
						  guest_port + offset, 2));
	const int after = open_descriptors();
	if (after != before) {
		fprintf(stderr,
			"port 0: %d host descriptor(s) open before, %d after\n",
			before, after);
		failures++;
	}

	/* The gateway reaches the host's loopback, which refuses port 0. */
	const Reply gateway = connect_attempt(network, gateway_ip, 0, guest_port);
	if (!gateway.tcp || (gateway.tcp->flags & tcp_rst) == 0 ||
	    gateway.tcp->source_port != 0 || gateway.icmp) {
		fprintf(stderr,
			"gateway port 0: expected a reset, got tcp=%d flags "
			"0x%02x, icmp=%d, %zu frame(s)\n",
			gateway.tcp ? 1 : 0, gateway.tcp ? gateway.tcp->flags : 0,
			gateway.icmp ? 1 : 0, gateway.frames);
		failures++;
	}

	/* A destination Darwin refuses synchronously for another reason keeps
	 * the ICMP error; Linux refuses a multicast TCP connect the same way,
	 * with ENETUNREACH. */
	const Reply multicast =
		connect_attempt(network, {224, 0, 0, 1}, 80, guest_port);
	if (!multicast.icmp ||
	    multicast.icmp_type != icmp_destination_unreachable ||
	    multicast.icmp_code != icmp_net_unreachable) {
		fprintf(stderr,
			"multicast: expected ICMP net-unreachable, got "
			"icmp=%d type %u code %u, %zu frame(s)\n",
			multicast.icmp ? 1 : 0,
			static_cast<unsigned int>(multicast.icmp_type),
			static_cast<unsigned int>(multicast.icmp_code),
			multicast.frames);
		failures++;
	}

	/* A refused port is still refused with a reset: the gateway reaches
	 * the host's loopback, where nothing listens on a fresh port. */
	const std::uint16_t closed_port = unused_loopback_port();
	if (closed_port == 0) {
		fprintf(stderr, "could not pick an unused loopback port\n");
		failures++;
	} else {
		const Reply refused = connect_attempt(network, gateway_ip,
						      closed_port, guest_port);
		if (!refused.tcp || (refused.tcp->flags & tcp_rst) == 0 ||
		    refused.tcp->source_port != closed_port) {
			fprintf(stderr,
				"closed port: expected a reset, got tcp=%d "
				"flags 0x%02x, icmp=%d, %zu frame(s)\n",
				refused.tcp ? 1 : 0,
				refused.tcp ? refused.tcp->flags : 0,
				refused.icmp ? 1 : 0, refused.frames);
			failures++;
		}
	}

	if (failures != 0)
		return 1;
	printf("PASS: a connect to port 0 stays unanswered and leaks no "
	       "socket, the gateway resets it, other synchronous failures "
	       "keep ICMP net-unreachable, and a closed port is reset\n");
	return 0;
}
