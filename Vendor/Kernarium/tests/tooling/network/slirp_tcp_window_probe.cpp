/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SLiRP's receive window must describe the buffer it has left. A guest may
 * legitimately advance SLiRP's next expected sequence past the edge SLiRP
 * last advertised: a FIN sent at a closed window is accepted, and so is a
 * one-octet window probe while SLiRP holds some space. The window SLiRP
 * advertises next must still come from that space, not from the negative
 * distance to the old edge read as an unsigned 32-bit value, which on LP64
 * hosts turns a full buffer into a 64 KiB window.
 *
 * The guest fills a connection to a host listener that never reads until
 * SLiRP closes its window, then half-closes it: the ACK of that FIN must
 * still advertise a closed window.
 */
#include "slirp_tcp_harness.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace {

using namespace slirp_harness;

constexpr std::uint16_t guest_port = 40000;
constexpr std::uint32_t guest_iss = 1000;

struct Receiver {
	std::uint32_t ack{};
	std::uint16_t window{};
	bool seen{};
	bool syn{};
	std::uint32_t syn_seq{};
};

void drain(Network &network, Receiver &receiver)
{
	while (!network.frames().empty()) {
		const auto segment = parse_tcp(network.frames().front());
		network.frames().pop_front();
		if (!segment || segment->destination_port != guest_port)
			continue;
		if ((segment->flags & tcp_syn) != 0) {
			receiver.syn = true;
			receiver.syn_seq = segment->seq;
		}
		if ((segment->flags & tcp_ack) != 0) {
			receiver.ack = segment->ack;
			receiver.window = segment->window;
			receiver.seen = true;
		}
	}
}

} // namespace

int main(void)
{
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fprintf(stderr, "SLiRP did not start or answer ARP\n");
		return 1;
	}

	/* A listener with a small buffer that never reads, so the loopback
	 * connection and then SLiRP's own buffer fill. */
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	const int small = 4096;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (listener < 0 ||
	    setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) != 0 ||
	    bind(listener, reinterpret_cast<const sockaddr *>(&address),
		 sizeof(address)) != 0 ||
	    listen(listener, 1) != 0 ||
	    getsockname(listener, reinterpret_cast<sockaddr *>(&address),
			&length) != 0) {
		fprintf(stderr, "could not listen on loopback\n");
		return 1;
	}
	const std::uint16_t host_port = ntohs(address.sin_port);

	Receiver receiver;
	network.input(tcp_frame(guest_ip, gateway_ip, guest_port, host_port,
				guest_iss, 0, tcp_syn, 65535));
	for (int pass = 0; pass < 200 && !receiver.syn; ++pass) {
		network.pump(10, 10);
		drain(network, receiver);
	}
	if (!receiver.syn || receiver.ack != guest_iss + 1) {
		fprintf(stderr, "SLiRP did not accept the guest's connection\n");
		return 1;
	}
	std::uint32_t next = guest_iss + 1;
	const std::uint32_t peer = receiver.syn_seq + 1;
	network.input(tcp_frame(guest_ip, gateway_ip, guest_port, host_port,
				next, peer, tcp_ack, 65535));
	drain(network, receiver);

	/* Send within every window SLiRP advertises until it stays closed. */
	int closed_passes = 0;
	for (int pass = 0; pass < 20000 && closed_passes < 50; ++pass) {
		const std::uint32_t edge = receiver.ack + receiver.window;
		while (static_cast<std::int32_t>(edge - next) > 0) {
			const std::uint32_t room = edge - next;
			const std::size_t size = room < 1460 ? room : 1460;
			network.input(tcp_frame(guest_ip, gateway_ip, guest_port,
						host_port, next, peer,
						tcp_ack | tcp_psh, 65535, size));
			next += static_cast<std::uint32_t>(size);
			drain(network, receiver);
		}
		network.pump(100, 1);
		drain(network, receiver);
		closed_passes = receiver.window == 0 && receiver.ack == next ?
			closed_passes + 1 : 0;
	}
	if (closed_passes < 50) {
		fprintf(stderr,
			"SLiRP never closed its window (ack %u of %u, window %u)\n",
			static_cast<unsigned int>(receiver.ack - guest_iss),
			static_cast<unsigned int>(next - guest_iss),
			static_cast<unsigned int>(receiver.window));
		return 1;
	}
	const std::uint32_t filled = next - guest_iss - 1;

	receiver.seen = false;
	network.input(tcp_frame(guest_ip, gateway_ip, guest_port, host_port,
				next, peer, tcp_fin | tcp_ack, 65535));
	drain(network, receiver);
	close(listener);
	if (!receiver.seen || receiver.ack != next + 1) {
		fprintf(stderr, "SLiRP did not acknowledge the guest's FIN\n");
		return 1;
	}
	if (receiver.window != 0) {
		fprintf(stderr,
			"after %u bytes filled SLiRP's closed window, the ACK of "
			"the guest's FIN advertised a %u-byte window\n",
			static_cast<unsigned int>(filled),
			static_cast<unsigned int>(receiver.window));
		return 1;
	}
	printf("PASS: SLiRP kept its receive window closed (0) after a FIN "
	       "passed its advertised edge; %u bytes filled it\n",
	       static_cast<unsigned int>(filled));
	return 0;
}
