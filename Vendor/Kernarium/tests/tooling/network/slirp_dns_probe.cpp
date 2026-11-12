/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest's name server is answered by the host's system resolver, and the
 * guest's DNS never makes SLiRP open a host Internet socket: SLiRP's own proxy
 * would send each query to the host's first configured server, which on most
 * networks is a local-network router.
 *
 * The probe runs the provider with the resolver confined to
 * kDNSServiceInterfaceIndexLocalOnly, which answers from /etc/hosts and gives
 * an immediate negative answer for any other name, so it needs no network.
 * It checks UDP and TCP, the name server and the gateway address, a missing
 * name, the refusals (a non-DNS TCP port is reset, a non-DNS datagram is
 * dropped), guest multicast, and a stop and reopen. The wire rules themselves
 * are the unit test network-dns-rules.
 */
#include "dns_message.hpp"
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"
#include "uml_slirp_net.h"

#include <dns_sd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;
using Clock = std::chrono::steady_clock;
namespace dns = uml::network::dns;

constexpr Address name_server{10, 0, 2, 3};
constexpr Address gateway{10, 0, 2, 2};

int failures;

void fail(const char *label, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

std::vector<std::uint8_t> wire(const char *presentation)
{
	return dns::wire_name(presentation).value_or(std::vector<std::uint8_t>{});
}

std::vector<std::uint8_t> query_message(std::uint16_t id, const char *name,
					std::uint16_t type, bool edns = false)
{
	std::vector<std::uint8_t> message(12, 0);
	put16(message, 0, id);
	put16(message, 2, 0x0100); /* RD */
	put16(message, 4, 1);
	put16(message, 10, edns ? 1 : 0);
	const auto encoded = wire(name);
	message.insert(message.end(), encoded.begin(), encoded.end());
	message.resize(message.size() + 4);
	put16(message, message.size() - 4, type);
	put16(message, message.size() - 2, 1);
	if (edns) {
		const std::uint8_t opt[] = {0, 0, 41, 0x04, 0xd0, 0, 0, 0, 0, 0, 0};
		message.insert(message.end(), std::begin(opt), std::end(opt));
	}
	return message;
}

std::vector<std::uint8_t> udp_frame(const Address &destination,
				    std::uint16_t source_port,
				    std::uint16_t destination_port,
				    const std::vector<std::uint8_t> &payload)
{
	const std::size_t udp_length = 8 + payload.size();
	std::vector<std::uint8_t> frame(14 + 20 + udp_length, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + udp_length));
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = IPPROTO_UDP;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = guest_ip[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));
	put16(frame, 34, source_port);
	put16(frame, 36, destination_port);
	put16(frame, 38, static_cast<std::uint16_t>(udp_length));
	std::copy(payload.begin(), payload.end(), frame.begin() + 42);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_UDP;
	sum += static_cast<std::uint32_t>(udp_length);
	sum = checksum_add(sum, &frame[34], udp_length);
	put16(frame, 40, checksum_fold(sum));
	return frame;
}

std::vector<std::uint8_t> tcp_data_frame(const Address &destination,
					 std::uint16_t source_port,
					 std::uint32_t seq, std::uint32_t ack,
					 std::uint8_t flags,
					 const std::vector<std::uint8_t> &payload)
{
	auto frame = tcp_frame(guest_ip, destination, source_port, 53, seq, ack,
			       flags, 65535, payload.size());
	std::copy(payload.begin(), payload.end(), frame.begin() + 54);
	put16(frame, 50, 0);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_TCP;
	sum += static_cast<std::uint32_t>(frame.size() - 34);
	sum = checksum_add(sum, &frame[34], frame.size() - 34);
	put16(frame, 50, checksum_fold(sum));
	return frame;
}

std::vector<std::uint8_t> arp_request()
{
	std::vector<std::uint8_t> frame(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
		frame[22 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0806);
	put16(frame, 14, 1);
	put16(frame, 16, 0x0800);
	frame[18] = 6;
	frame[19] = 4;
	put16(frame, 20, 1);
	std::copy(guest_ip.begin(), guest_ip.end(), frame.begin() + 28);
	std::copy(gateway.begin(), gateway.end(), frame.begin() + 38);
	return frame;
}

class Guest final {
public:
	explicit Guest(uml::network::SlirpProvider &provider)
		: provider_(provider)
	{
	}

	bool send(const std::vector<std::uint8_t> &frame)
	{
		return provider_.write({reinterpret_cast<const std::byte *>(
						frame.data()),
					frame.size()}) == 0;
	}

	/* The next frame the provider delivers that `accept` takes. */
	template <class Accept>
	std::optional<std::vector<std::uint8_t>> receive(Accept accept,
							 int timeout_ms = 3000)
	{
		const auto deadline =
			Clock::now() + std::chrono::milliseconds{timeout_ms};
		std::array<std::byte, 2048> buffer{};
		while (Clock::now() < deadline) {
			const int length = provider_.read(buffer);
			if (length <= 0) {
				pollfd descriptor{provider_.irq_fd(), POLLIN, 0};
				(void)::poll(&descriptor, 1, 10);
				continue;
			}
			std::vector<std::uint8_t> frame(
				reinterpret_cast<const std::uint8_t *>(buffer.data()),
				reinterpret_cast<const std::uint8_t *>(buffer.data()) +
					length);
			if (accept(frame))
				return frame;
		}
		return std::nullopt;
	}

private:
	uml::network::SlirpProvider &provider_;
};

struct Reply {
	std::uint16_t id{};
	std::uint8_t rcode{};
	bool truncated{};
	std::uint16_t answers{};
	std::vector<std::uint8_t> message;
};

std::optional<Reply> parse_reply(const std::vector<std::uint8_t> &message)
{
	if (message.size() < 12 || (message[2] & 0x80) == 0)
		return std::nullopt;
	return Reply{get16(message, 0), static_cast<std::uint8_t>(message[3] & 0x0f),
		     (message[2] & 0x02) != 0, get16(message, 6), message};
}

/* A UDP DNS reply from `server`:53 to `port`. */
std::optional<Reply> udp_reply(const std::vector<std::uint8_t> &frame,
			       const Address &server, std::uint16_t port)
{
	if (frame.size() < 42 || get16(frame, 12) != 0x0800 ||
	    frame[23] != IPPROTO_UDP || get16(frame, 34) != 53 ||
	    get16(frame, 36) != port)
		return std::nullopt;
	for (std::size_t index = 0; index < 4; ++index)
		if (frame[26 + index] != server[index] ||
		    frame[30 + index] != guest_ip[index])
			return std::nullopt;
	return parse_reply({frame.begin() + 42, frame.end()});
}

bool has_loopback_answer(const Reply &reply)
{
	const std::uint8_t loopback[] = {0, 1, 0, 1, 0, 0, 0, 0, 0, 4,
					 127, 0, 0, 1};
	/* Type A, class IN, any TTL, RDLENGTH 4, 127.0.0.1. */
	for (std::size_t index = 0; index + sizeof(loopback) <= reply.message.size();
	     ++index) {
		bool match = true;
		for (std::size_t byte = 0; byte < sizeof(loopback); ++byte) {
			if (byte >= 4 && byte < 8)
				continue;
			if (reply.message[index + byte] != loopback[byte]) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

void check_udp(uml::network::SlirpProvider &provider, Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 40000;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"UDP to the name server" : "UDP to the gateway";
		++port;
		const std::uint16_t id = static_cast<std::uint16_t>(0x5100 + port);
		const auto started = Clock::now();
		if (!guest.send(udp_frame(server, port, 53,
					  query_message(id, "localhost", 1))))
			fail(label, "the provider refused the query");
		std::optional<Reply> reply;
		(void)guest.receive([&](const std::vector<std::uint8_t> &frame) {
			reply = udp_reply(frame, server, port);
			return reply.has_value();
		});
		if (!reply || reply->id != id || reply->rcode != 0 ||
		    !has_loopback_answer(*reply)) {
			fail(label, "localhost has no 127.0.0.1 answer");
			continue;
		}
		std::printf("%s: localhost answered in %lld ms\n", label,
			    static_cast<long long>(
				    std::chrono::duration_cast<
					    std::chrono::milliseconds>(
					    Clock::now() - started)
					    .count()));
	}

	const char *label = "a missing name";
	if (!guest.send(udp_frame(name_server, 40100, 53,
				  query_message(0x6200,
						"uml-dns-probe-missing.invalid",
						1))))
		fail(label, "the provider refused the query");
	std::optional<Reply> reply;
	(void)guest.receive([&](const std::vector<std::uint8_t> &frame) {
		reply = udp_reply(frame, name_server, 40100);
		return reply.has_value();
	});
	if (!reply || reply->answers != 0 ||
	    (reply->rcode != dns::rcode_noerror &&
	     reply->rcode != dns::rcode_nxdomain))
		fail(label, "no negative answer");
	else
		std::printf("%s: rcode %u\n", label, reply->rcode);

	label = "guest multicast";
	const Address mdns{224, 0, 0, 251};
	const auto before_multicast = provider.internet_sockets_for_testing();
	if (!guest.send(udp_frame(mdns, 5353, 5353, query_message(0, "adb.example", 255))))
		fail(label, "the provider refused the datagram");
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	if (provider.internet_sockets_for_testing() != before_multicast)
		fail(label, "an mDNS datagram left the virtual link");

	label = "a datagram to another port";
	if (!guest.send(udp_frame(name_server, 40101, 123,
				  std::vector<std::uint8_t>(48, 0))))
		fail(label, "the provider refused the datagram");
	if (guest.receive([](const std::vector<std::uint8_t> &frame) {
		    return frame.size() > 42 && frame[23] == IPPROTO_UDP &&
			    get16(frame, 36) == 40101;
	    }, 300))
		fail(label, "it was answered");

	if (provider.internet_sockets_for_testing() != internet)
		fail("UDP", "the guest's DNS made SLiRP open a host Internet socket");

	/* The witness itself: a datagram SLiRP does handle, to the gateway's
	 * discard port, which it sends to host loopback, is counted. */
	label = "the socket witness";
	if (!guest.send(udp_frame(gateway, 40102, 9,
				  std::vector<std::uint8_t>(8, 0))))
		fail(label, "the provider refused the datagram");
	const auto deadline = Clock::now() + std::chrono::seconds{2};
	while (provider.internet_sockets_for_testing() == internet &&
	       Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	if (provider.internet_sockets_for_testing() == internet)
		fail(label, "a datagram SLiRP forwards was not counted");
}

/* One DNS-over-TCP exchange with `server`:53; returns the reply. With
 * `half_close` the guest sends its FIN with the query, as a client does that
 * shuts down its sending side and then reads. */
std::optional<Reply> tcp_exchange(Guest &guest, const Address &server,
				  std::uint16_t port,
				  const std::vector<std::uint8_t> &message,
				  bool half_close = false)
{
	const auto from_server = [&](const std::vector<std::uint8_t> &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source == server &&
			segment->source_port == 53 &&
			segment->destination_port == port;
	};
	std::uint32_t seq = 1000;
	if (!guest.send(tcp_frame(guest_ip, server, port, 53, seq, 0, tcp_syn,
				  65535)))
		return std::nullopt;
	const auto synack = guest.receive(from_server);
	if (!synack)
		return std::nullopt;
	const auto handshake = *parse_tcp(*synack);
	if ((handshake.flags & (tcp_syn | tcp_ack)) != (tcp_syn | tcp_ack))
		return std::nullopt;
	++seq;
	std::uint32_t ack = handshake.seq + 1;
	std::vector<std::uint8_t> framed;
	framed.push_back(static_cast<std::uint8_t>(message.size() >> 8));
	framed.push_back(static_cast<std::uint8_t>(message.size()));
	framed.insert(framed.end(), message.begin(), message.end());
	if (!guest.send(tcp_data_frame(server, port, seq, ack,
				       tcp_ack | tcp_psh |
					       (half_close ? tcp_fin : 0),
				       framed)))
		return std::nullopt;
	seq += static_cast<std::uint32_t>(framed.size()) + (half_close ? 1 : 0);

	std::vector<std::uint8_t> stream;
	while (stream.size() < 2 ||
	       stream.size() < 2U + get16(stream, 0)) {
		const auto frame = guest.receive(from_server);
		if (!frame)
			return std::nullopt;
		const auto segment = *parse_tcp(*frame);
		if ((segment.flags & tcp_rst) != 0)
			return std::nullopt;
		if (segment.payload == 0)
			continue;
		const std::size_t offset = frame->size() - segment.payload;
		stream.insert(stream.end(), frame->begin() + offset, frame->end());
		ack = segment.seq + static_cast<std::uint32_t>(segment.payload);
		(void)guest.send(tcp_frame(guest_ip, server, port, 53, seq, ack,
					   tcp_ack, 65535));
	}
	if (!half_close)
		(void)guest.send(tcp_frame(guest_ip, server, port, 53, seq, ack,
					   tcp_fin | tcp_ack, 65535));
	return parse_reply({stream.begin() + 2, stream.end()});
}

void check_tcp(uml::network::SlirpProvider &provider, Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 41000;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"TCP to the name server" : "TCP to the gateway";
		const auto reply = tcp_exchange(guest, server, ++port,
						query_message(0x7300, "localhost", 1));
		if (!reply || reply->id != 0x7300 || reply->rcode != 0 ||
		    !has_loopback_answer(*reply))
			fail(label, "localhost has no 127.0.0.1 answer");
	}

	const char *label = "TCP half-close";
	const auto half = tcp_exchange(guest, name_server, ++port,
				       query_message(0x7301, "localhost", 1),
				       true);
	if (!half || half->id != 0x7301 || half->rcode != 0 ||
	    !has_loopback_answer(*half))
		fail(label, "a client that shut down its sending side got no answer");

	label = "DNS over TLS";
	if (!guest.send(tcp_frame(guest_ip, name_server, 41100, 853, 5000, 0,
				  tcp_syn, 65535)))
		fail(label, "the provider refused the SYN");
	const auto reset = guest.receive([](const std::vector<std::uint8_t> &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source_port == 853 &&
			segment->destination_port == 41100;
	});
	const auto segment = reset ? parse_tcp(*reset) : std::nullopt;
	if (!segment || segment->flags != (tcp_rst | tcp_ack) ||
	    segment->ack != 5001)
		fail(label, "port 853 of the name server is not reset");

	if (provider.internet_sockets_for_testing() != internet)
		fail("TCP", "the guest's DNS made SLiRP open a host Internet socket");
}

/* Without a socket path for DNS over TCP, a connection to port 53 is reset
 * like one to any other port: SLiRP, with its own proxy off, would connect to
 * the literal address on the host's network. */
void check_tcp_without_stream(uml::network::SlirpProvider &provider,
			      Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 41200;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"TCP without a stream socket, name server" :
			"TCP without a stream socket, gateway";
		++port;
		if (!guest.send(tcp_frame(guest_ip, server, port, 53, 7000, 0,
					  tcp_syn, 65535)))
			fail(label, "the provider refused the SYN");
		const auto answer = guest.receive(
			[&](const std::vector<std::uint8_t> &frame) {
				const auto segment = parse_tcp(frame);
				return segment && segment->source == server &&
					segment->source_port == 53 &&
					segment->destination_port == port;
			});
		const auto segment = answer ? parse_tcp(*answer) : std::nullopt;
		if (!segment || segment->flags != (tcp_rst | tcp_ack) ||
		    segment->ack != 7001)
			fail(label, "port 53 is not reset");
	}
	if (provider.internet_sockets_for_testing() != internet)
		fail("TCP without a stream socket",
		     "SLiRP opened a host Internet socket");
}

bool open_provider(uml::network::SlirpProvider &provider, Guest &guest)
{
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	if (provider.open(mac, mtu) != 0)
		return false;
	(void)guest.send(arp_request());
	(void)guest.receive([](const std::vector<std::uint8_t> &frame) {
		return frame.size() >= 14 && get16(frame, 12) == 0x0806;
	});
	return true;
}

/* Where the resolver bound its DNS-over-TCP socket, for the log of a run on a
 * device or in the Simulator. */
void report_stream(const char *label, uml::network::SlirpProvider &provider)
{
	auto &resolver = provider.resolver_for_testing();
	std::printf("%s: DNS over TCP %s\n", label,
		    resolver.streams_available() ?
			    resolver.stream_path().c_str() : "is unavailable");
}

bool starts_with(const std::string &text, const std::string &prefix)
{
	return text.compare(0, prefix.size(), prefix) == 0;
}

/* The socket for DNS over TCP goes where $TMPDIR names, when that leaves room
 * in sun_path (an iOS device's container tmp does), and in a shorter
 * directory when it does not (an iOS Simulator's does not). */
void check_stream_directory(uml::network::SlirpProvider &provider, Guest &guest,
			    const char *label, const std::string &directory,
			    bool expect_inside)
{
	::setenv("TMPDIR", directory.c_str(), 1);
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
		return;
	}
	report_stream(label, provider);
	auto &resolver = provider.resolver_for_testing();
	if (!resolver.streams_available())
		fail(label, "the resolver has no DNS-over-TCP listener");
	else if (starts_with(resolver.stream_path(), directory) != expect_inside)
		fail(label, expect_inside ?
			"the socket is not in $TMPDIR" :
			"the socket is in a directory too long for sun_path");
	else if (resolver.stream_path().size() >= sizeof(sockaddr_un::sun_path))
		fail(label, "the socket path does not fit sun_path");
	check_tcp(provider, guest);
	provider.stop();
}

/* A Unix socket's name left in the file system, as a killed process leaves. */
void leave_socket(const std::string &path)
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		return;
	(void)::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
		     sizeof(address));
	(void)::close(descriptor);
}

/* The pid of a process that has exited and been collected. */
pid_t exited_process()
{
	const pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	int status = 0;
	if (child > 0)
		(void)::waitpid(child, &status, 0);
	return child;
}

bool exists(const std::string &path)
{
	struct stat details{};
	return ::lstat(path.c_str(), &details) == 0;
}

/* A run killed outright leaves its DNS-over-TCP socket. The next run in that
 * directory removes it, and nothing else: not a live process's socket, not a
 * file that is no socket, not another name. */
void check_abandoned_streams(uml::network::SlirpProvider &provider,
			     Guest &guest)
{
	const char *label = "a killed run's DNS-over-TCP socket";
	char base[] = "/tmp/uml-dns-probe.XXXXXX";
	if (::mkdtemp(base) == nullptr) {
		fail(label, "no scratch directory");
		return;
	}
	const std::string directory = std::string(base) + "/";
	const std::string prefix{uml::network::dns_stream_prefix};
	const pid_t dead = exited_process();
	const pid_t other_dead = exited_process();
	const auto stale = directory + prefix + std::to_string(dead);
	const auto live = directory + prefix + std::to_string(::getppid());
	const auto regular = directory + prefix + std::to_string(other_dead);
	const auto unrelated = directory + "mconsole";
	leave_socket(stale);
	leave_socket(live);
	leave_socket(unrelated);
	(void)::close(::open(regular.c_str(), O_CREAT | O_WRONLY, 0600));

	::setenv("TMPDIR", directory.c_str(), 1);
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
	} else {
		const std::string own = provider.resolver_for_testing().stream_path();
		if (exists(stale))
			fail(label, "the socket of a process that exited was kept");
		if (!exists(live))
			fail(label, "the socket of a live process was removed");
		if (!exists(regular))
			fail(label, "a file that is no socket was removed");
		if (!exists(unrelated))
			fail(label, "a socket of another name was removed");
		if (!starts_with(own, directory) || !exists(own))
			fail(label, "the run has no socket of its own");
		provider.stop();
		if (exists(own))
			fail(label, "the run's socket outlived it");
	}
	std::error_code ignored;
	std::filesystem::remove_all(base, ignored);
}

} // namespace

int main()
{
	const char *original = std::getenv("TMPDIR");
	const std::string saved_directory = original != nullptr ? original : "";
	uml::network::SlirpProvider provider;
	Guest guest{provider};

	if (!open_provider(provider, guest)) {
		std::fprintf(stderr, "the provider did not open\n");
		return 1;
	}
	if (!provider.resolver_for_testing().streams_available())
		fail("TCP", "the resolver has no DNS-over-TCP listener");
	report_stream(saved_directory.empty() ? "default $TMPDIR (unset)" :
			      "default $TMPDIR", provider);
	check_udp(provider, guest);
	check_tcp(provider, guest);

	/* A guest restart: the resolver comes back with the link. */
	provider.stop();
	if (!open_provider(provider, guest)) {
		std::fprintf(stderr, "the provider did not reopen\n");
		return 1;
	}
	check_udp(provider, guest);
	provider.stop();

	provider.set_dns_streams_unavailable_for_testing(true);
	if (open_provider(provider, guest)) {
		if (provider.resolver_for_testing().streams_available())
			fail("TCP without a stream socket",
			     "a listener exists though no path was offered");
		check_tcp_without_stream(provider, guest);
		provider.stop();
	} else {
		fail("TCP without a stream socket", "the provider did not open");
	}
	provider.set_dns_streams_unavailable_for_testing(false);

	/* An iOS device's container tmp is 89 bytes and the socket's name fits
	 * it; the Simulator's is about 164 and does not. */
	char base[] = "/tmp/uml-dns-probe.XXXXXX";
	if (::mkdtemp(base) == nullptr) {
		fail("stream directory", "no scratch directory");
	} else {
		const std::string container = std::string(base) + "/" +
			std::string(52, 't') + "/";
		if (::mkdir((container.substr(0, container.size() - 1)).c_str(),
			    0700) != 0)
			fail("stream directory", "no container directory");
		else
			check_stream_directory(provider, guest,
					       "a container tmp that fits",
					       container, true);
		std::error_code ignored;
		std::filesystem::remove_all(base, ignored);
	}
	check_stream_directory(provider, guest, "a container tmp too long",
			       "/" + std::string(150, 'l') + "/tmp/", false);
	check_abandoned_streams(provider, guest);

	if (original != nullptr)
		::setenv("TMPDIR", saved_directory.c_str(), 1);
	else
		::unsetenv("TMPDIR");

	if (failures != 0) {
		std::fprintf(stderr, "%d DNS check(s) failed\n", failures);
		return 1;
	}
	std::puts("guest DNS is answered by the system resolver");
	return 0;
}
