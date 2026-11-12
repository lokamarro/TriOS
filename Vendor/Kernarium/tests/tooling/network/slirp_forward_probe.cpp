/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host-forward specification is the only user-facing grammar the user
 * network exposes, and a silently misread forward is worse than a refused one:
 * the guest port simply never answers. Pin every accepted form, every default,
 * and the malformed shapes that must be refused, and when a forward may join
 * a SLiRP instance.
 */
#include "provider.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

int failures;

void expect_rejected(std::string_view specification)
{
	uml::network::HostForward forward{};

	if (!uml::network::parse_host_forward(specification, forward))
		return;
	fprintf(stderr, "accepted malformed forward '%.*s'\n",
		static_cast<int>(specification.size()), specification.data());
	failures++;
}

void expect_parsed(std::string_view specification, bool udp,
		   std::string_view host_address, unsigned int host_port,
		   std::string_view guest_address, unsigned int guest_port)
{
	uml::network::HostForward forward{};

	if (!uml::network::parse_host_forward(specification, forward)) {
		fprintf(stderr, "rejected valid forward '%.*s'\n",
			static_cast<int>(specification.size()),
			specification.data());
		failures++;
		return;
	}
	if (forward.udp == udp && forward.host_address == host_address &&
	    forward.host_port == host_port &&
	    forward.guest_address == guest_address &&
	    forward.guest_port == guest_port)
		return;
	fprintf(stderr,
		"forward '%.*s' parsed as %s %s:%u-%s:%u\n",
		static_cast<int>(specification.size()), specification.data(),
		forward.udp ? "udp" : "tcp", forward.host_address.c_str(),
		static_cast<unsigned int>(forward.host_port),
		forward.guest_address.c_str(),
		static_cast<unsigned int>(forward.guest_port));
	failures++;
}

/* A loopback port nothing listens on, found by letting the host pick one. */
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

void expect(bool condition, const char *what)
{
	if (condition)
		return;
	fprintf(stderr, "%s\n", what);
	failures++;
}

/* libslirp takes no lock, and its worker walks the socket lists a forward is
 * linked into (and, when the host port is taken, freed from). Every forward
 * therefore joins an instance before that instance's worker starts, and none
 * joins one that is running. The taken port exercises the path that frees. */
void forwards_join_before_the_worker_runs()
{
	const int taken = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	expect(taken >= 0 &&
		       bind(taken, reinterpret_cast<const sockaddr *>(&address),
			    sizeof(address)) == 0 &&
		       listen(taken, 1) == 0 &&
		       getsockname(taken, reinterpret_cast<sockaddr *>(&address),
				   &length) == 0,
	       "could not hold a loopback port");

	uml::network::SlirpProvider provider;
	uml::network::HostForward forward{};
	forward.host_port = unused_loopback_port();
	forward.guest_port = 22;
	expect(forward.host_port != 0, "no free loopback port");
	expect(provider.add_forward(forward) == 0,
	       "a forward registered before open was refused");
	uml::network::HostForward busy{};
	busy.host_port = ntohs(address.sin_port);
	busy.guest_port = 80;
	expect(provider.add_forward(busy) == 0,
	       "a forward on a taken port was refused before open");

	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	expect(provider.open(mac, mtu) == 0, "the provider did not open");
	expect(provider.forwards_installed_while_running_for_testing() == 0,
	       "a forward was installed while the SLiRP worker ran");
	uml::network::HostForward late{};
	late.host_port = unused_loopback_port();
	late.guest_port = 23;
	expect(provider.add_forward(late) == -EBUSY,
	       "a forward was accepted into a running instance");

	provider.stop();
	expect(provider.add_forward(late) == 0,
	       "a stopped provider refused a forward");
	expect(provider.open(mac, mtu) == 0, "the provider did not reopen");
	expect(provider.forwards_installed_while_running_for_testing() == 0,
	       "a reopen installed a forward while the SLiRP worker ran");
	provider.stop();
	if (taken >= 0)
		close(taken);
}

/* Loopback ports nothing listens on, all distinct: every socket stays bound
 * until all are picked. */
std::vector<std::uint16_t> unused_loopback_ports(std::size_t count)
{
	std::vector<int> descriptors;
	std::vector<std::uint16_t> ports;
	for (std::size_t index = 0; index < count; ++index) {
		const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor < 0)
			break;
		descriptors.push_back(descriptor);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t length = sizeof(address);
		if (bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
			 sizeof(address)) != 0 ||
		    getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
				&length) != 0)
			break;
		ports.push_back(ntohs(address.sin_port));
	}
	for (const int descriptor : descriptors)
		close(descriptor);
	return ports;
}

/* SLiRP learns about a forwarded connection only when the event loop polls
 * that forward's listener, so every listener has to be in the poll set
 * however many sockets the guest's flows add. The forwards are made the
 * sockets here because a test can hold hundreds of them without a guest.
 * A connection to the first and to the last one registered must each make
 * SLiRP reach for the guest, which puts a frame on the guest side. */
void every_forward_listener_is_polled(bool first)
{
	constexpr std::size_t forwards = 600;
	const auto ports = unused_loopback_ports(forwards);
	expect(ports.size() == forwards, "could not pick enough loopback ports");
	if (ports.size() != forwards)
		return;

	uml::network::SlirpProvider provider;
	for (const auto port : ports) {
		uml::network::HostForward forward{};
		forward.host_port = port;
		forward.guest_port = 7;
		expect(provider.add_forward(forward) == 0,
		       "a forward registered before open was refused");
	}
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) != 0) {
		expect(false, "the provider did not open with many forwards");
		return;
	}
	pollfd irq{provider.irq_fd(), POLLIN, 0};
	expect(poll(&irq, 1, 200) == 0,
	       "the provider sent a frame before any connection");

	const int client = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(first ? ports.front() : ports.back());
	expect(client >= 0 &&
		       connect(client, reinterpret_cast<const sockaddr *>(&address),
			       sizeof(address)) == 0,
	       "could not connect to a forwarded port");
	irq.revents = 0;
	expect(poll(&irq, 1, 5000) == 1 && (irq.revents & POLLIN) != 0,
	       first ? "the first forward's connection never reached SLiRP" :
		       "the last forward's connection never reached SLiRP");
	if (client >= 0)
		close(client);
	provider.stop();
}

} // namespace

int main(void)
{
	/* The short form is the one an operator types: TCP, bound to loopback so
	 * the forward is not reachable off the machine, aimed at the DHCP lease. */
	expect_parsed("5555-5555", false, "127.0.0.1", 5555, "10.0.2.15", 5555);
	expect_parsed("8022-22", false, "127.0.0.1", 8022, "10.0.2.15", 22);
	expect_parsed("tcp:5555-5555", false, "127.0.0.1", 5555, "10.0.2.15",
		      5555);
	expect_parsed("udp:5353-5353", true, "127.0.0.1", 5353, "10.0.2.15",
		      5353);
	/* Binding off loopback has to be spelled out, never inferred. */
	expect_parsed("tcp:0.0.0.0:5555-10.0.2.15:5555", false, "0.0.0.0", 5555,
		      "10.0.2.15", 5555);
	expect_parsed("192.168.64.1:80-10.0.2.15:8080", false, "192.168.64.1",
		      80, "10.0.2.15", 8080);
	expect_parsed("udp:127.0.0.1:69-69", true, "127.0.0.1", 69, "10.0.2.15",
		      69);

	expect_rejected("");
	expect_rejected("5555");
	expect_rejected("-5555");
	expect_rejected("5555-");
	expect_rejected("0-5555");
	expect_rejected("5555-0");
	expect_rejected("65536-5555");
	expect_rejected("5555-65536");
	expect_rejected("sctp:5555-5555");
	expect_rejected("tcp:5555");
	expect_rejected("nothost:5555-5555");
	expect_rejected("127.0.0.1:-5555");
	expect_rejected("5555x-5555");
	expect_rejected("5555-5555x");
	expect_rejected("::1:5555-5555");

	forwards_join_before_the_worker_runs();

	/* Each forward holds a listening socket, more than the default
	 * descriptor limit allows. */
	rlimit files{};
	if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < 2048 &&
	    files.rlim_max >= 2048) {
		files.rlim_cur = 2048;
		(void)setrlimit(RLIMIT_NOFILE, &files);
	}
	every_forward_listener_is_polled(true);
	every_forward_listener_is_polled(false);

	if (failures != 0) {
		fprintf(stderr, "%d host-forward expectations failed\n",
			failures);
		return 1;
	}
	printf("PASS: host-forward specifications parse with loopback and "
	       "DHCP-lease defaults and refuse malformed forms, forwards "
	       "join an instance only before its worker runs, and every "
	       "forward's listener is polled\n");
	return 0;
}
