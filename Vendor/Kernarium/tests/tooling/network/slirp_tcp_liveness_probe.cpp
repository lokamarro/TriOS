/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A forwarded host connection must not outlive a guest that stops answering.
 * SLiRP holds the host end while it has data for the guest, so a guest that
 * vanished leaves the host client waiting until SLiRP gives up on it. Both
 * ways SLiRP waits must give up in bounded time: retransmitting data the
 * guest never acknowledges, and probing a window the guest closed and never
 * reopens. A guest that answers every probe is alive, and SLiRP must keep its
 * closed window open however long its reader stalls. The clock is SLiRP's
 * own, advanced one 500 ms tick per pass.
 */
#include "slirp_tcp_harness.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using namespace slirp_harness;

constexpr std::uint16_t guest_port = 7;
constexpr std::uint32_t guest_iss = 5000;
/* Keep-alive's give-up time for a silent peer, TCP_MAXIDLE, is 10 minutes;
 * allow one more maximum-length (60 s) probe interval past it. */
constexpr std::int64_t give_up_bound_ms = 11 * 60 * 1000;
constexpr std::int64_t observation_ms = 40 * 60 * 1000;

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

std::optional<Segment> next_segment(Network &network)
{
	while (!network.frames().empty()) {
		auto segment = parse_tcp(network.frames().front());
		network.frames().pop_front();
		if (segment && segment->destination_port == guest_port)
			return segment;
	}
	return std::nullopt;
}

/* True once the host client sees its connection closed or reset. */
bool host_closed(int client)
{
	pollfd entry{client, POLLIN, 0};
	if (poll(&entry, 1, 0) != 1)
		return false;
	char byte{};
	const auto received = recv(client, &byte, 1, MSG_DONTWAIT);
	return received == 0 || (received < 0 && errno != EAGAIN);
}

/* Forward a host connection to a guest that answers the handshake with
 * `window` and acknowledges the first data segment with `ack_window` when
 * `acknowledge` is set. It then goes silent, or with `answer_probes` answers
 * every later segment with the same zero-window acknowledgement, as a live
 * guest whose reader has stalled does. Returns how long after that SLiRP
 * closed the host end, or -1 if it held it for the whole observation. */
std::int64_t hold_time(const char *label, std::uint16_t window,
		       bool acknowledge, std::uint16_t ack_window,
		       bool answer_probes = false)
{
	Network network;
	const std::uint16_t host_port = unused_loopback_port();
	in_addr loopback{htonl(INADDR_LOOPBACK)};
	in_addr guest{};
	(void)inet_pton(AF_INET, "10.0.2.15", &guest);
	if (network.slirp() == nullptr || host_port == 0 || !network.resolve() ||
	    slirp_add_hostfwd(network.slirp(), 0, loopback, host_port, guest,
			      guest_port) != 0) {
		fprintf(stderr, "%s: could not forward a loopback port\n", label);
		return -2;
	}
	const int client = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(host_port);
	if (client < 0 ||
	    connect(client, reinterpret_cast<const sockaddr *>(&address),
		    sizeof(address)) != 0) {
		fprintf(stderr, "%s: could not connect to the forward\n", label);
		return -2;
	}

	std::optional<Segment> syn;
	for (int pass = 0; pass < 200 && !syn; ++pass) {
		network.pump(10, 10);
		while (auto segment = next_segment(network))
			if ((segment->flags & (tcp_syn | tcp_ack)) == tcp_syn)
				syn = segment;
	}
	if (!syn) {
		fprintf(stderr, "%s: SLiRP never sent the guest a SYN\n", label);
		close(client);
		return -2;
	}
	const auto guest_address = syn->destination;
	const auto slirp_address = syn->source;
	const std::uint16_t slirp_port = syn->source_port;
	network.input(tcp_frame(guest_address, slirp_address, guest_port,
				slirp_port, guest_iss, syn->seq + 1,
				tcp_syn | tcp_ack, window));

	const std::vector<char> data(4096, 'x');
	if (send(client, data.data(), data.size(), 0) !=
	    static_cast<ssize_t>(data.size())) {
		fprintf(stderr, "%s: the host client could not send\n", label);
		close(client);
		return -2;
	}
	std::optional<Segment> first;
	for (int pass = 0; pass < 200 && !first; ++pass) {
		network.pump(10, 10);
		while (auto segment = next_segment(network))
			if (segment->payload != 0 && !first)
				first = segment;
	}
	if (!first) {
		fprintf(stderr, "%s: SLiRP never sent the guest data\n", label);
		close(client);
		return -2;
	}
	if (acknowledge)
		network.input(tcp_frame(guest_address, slirp_address, guest_port,
					slirp_port, guest_iss + 1,
					first->seq +
						static_cast<std::uint32_t>(first->payload),
					tcp_ack, ack_window));

	const std::int64_t silent_from = network.now_ms();
	std::int64_t closed_after = -1;
	while (network.now_ms() - silent_from < observation_ms) {
		network.pump(500);
		bool probed = false;
		while (next_segment(network))
			probed = true;
		if (answer_probes && probed)
			network.input(tcp_frame(
				guest_address, slirp_address, guest_port, slirp_port,
				guest_iss + 1,
				first->seq + static_cast<std::uint32_t>(first->payload),
				tcp_ack, 0));
		if (host_closed(client)) {
			closed_after = network.now_ms() - silent_from;
			break;
		}
	}
	close(client);
	return closed_after;
}

int failures;

void expect_given_up(const char *label, std::int64_t held)
{
	if (held == -2) {
		failures++;
		return;
	}
	if (held < 0) {
		fprintf(stderr,
			"%s: SLiRP still held the host connection after %lld "
			"silent minutes\n",
			label, static_cast<long long>(observation_ms / 60000));
		failures++;
		return;
	}
	if (held > give_up_bound_ms) {
		fprintf(stderr, "%s: SLiRP held the host connection %lld s\n",
			label, static_cast<long long>(held / 1000));
		failures++;
		return;
	}
	printf("%s: SLiRP closed the host connection %lld s after the guest "
	       "went silent\n",
	       label, static_cast<long long>(held / 1000));
}

} // namespace

int main(void)
{
	expect_given_up("unacknowledged data",
			hold_time("unacknowledged data", 65535, false, 0));
	expect_given_up("closed window",
			hold_time("closed window", 1460, true, 0));
	/* A guest that keeps its window closed but answers every probe is
	 * alive, however long its reader stalls. */
	const std::int64_t answered =
		hold_time("answered probes", 1460, true, 0, true);
	if (answered != -1) {
		if (answered >= 0)
			fprintf(stderr,
				"answered probes: SLiRP closed a live guest's "
				"connection after %lld s\n",
				static_cast<long long>(answered / 1000));
		failures++;
	} else {
		printf("answered probes: SLiRP held a live guest's closed "
		       "window for %lld minutes\n",
		       static_cast<long long>(observation_ms / 60000));
	}
	if (failures != 0)
		return 1;
	printf("PASS: SLiRP gives up a silent guest's forwarded connection "
	       "within %lld s, retransmitting or probing a closed window, and "
	       "keeps a live guest's closed window open\n",
	       static_cast<long long>(give_up_bound_ms / 1000));
	return 0;
}
