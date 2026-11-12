/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * How SLiRP treats a guest's ICMP echo, and which packets it refuses.
 *
 * An echo to the gateway is answered by SLiRP itself, with a full TTL. An echo
 * the guest sent with a TTL of one expires at the SLiRP hop, as it would at a
 * router: SLiRP answers with ICMP time exceeded from the gateway and sends
 * nothing to the host. Any other echo goes out on a host ICMP socket with the
 * guest's TTL less one, and its reply reaches the guest with the TTL it
 * arrived with, not a reset one. The host's own address answers a ping with
 * its stack's TTL, which makes the reply a witness that the host kept the IP
 * header of a datagram ICMP socket's reply.
 *
 * SLiRP drops a packet whose source or destination no network carries:
 * loopback, class E (the limited broadcast excepted), the unspecified
 * address as a destination and a multicast source. A guest still sends a
 * DHCP DISCOVER from the unspecified address to the limited broadcast, and
 * that must be answered, or no guest gets an address.
 */
#include "slirp_tcp_harness.hpp"

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;

constexpr std::uint8_t icmp_echo_reply = 0;
constexpr std::uint8_t icmp_echo_request = 8;
constexpr std::uint8_t icmp_time_exceeded = 11;
constexpr std::uint16_t echo_identifier = 0x4a17;

int failures;

void fail(const char *label, const std::string &message)
{
	fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

/* One IPv4 packet SLiRP sent to the guest. */
struct Packet {
	Address source{};
	Address destination{};
	std::uint8_t protocol{};
	std::uint8_t ttl{};
	std::vector<std::uint8_t> payload;
};

std::optional<Packet> parse_ip(const std::vector<std::uint8_t> &frame)
{
	if (frame.size() < 34 || get16(frame, 12) != 0x0800)
		return std::nullopt;
	const std::size_t header = (frame[14] & 0x0f) * 4u;
	const std::size_t length = get16(frame, 16);
	if (length < header || frame.size() < 14 + length)
		return std::nullopt;
	Packet packet;
	for (std::size_t index = 0; index < 4; ++index) {
		packet.source[index] = frame[26 + index];
		packet.destination[index] = frame[30 + index];
	}
	packet.protocol = frame[23];
	packet.ttl = frame[22];
	packet.payload.assign(frame.begin() + 14 + header,
			      frame.begin() + 14 + length);
	return packet;
}

/* A guest IPv4 packet; the Ethernet destination is the broadcast address,
 * which the harness uses throughout. */
std::vector<std::uint8_t> ip_frame(const Address &source,
				   const Address &destination,
				   std::uint8_t protocol, std::uint8_t ttl,
				   const std::vector<std::uint8_t> &payload)
{
	std::vector<std::uint8_t> frame(14 + 20 + payload.size(), 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + payload.size()));
	put16(frame, 20, 0x4000);
	frame[22] = ttl;
	frame[23] = protocol;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = source[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));
	std::copy(payload.begin(), payload.end(), frame.begin() + 34);
	return frame;
}

std::vector<std::uint8_t> echo_payload(std::uint8_t type, std::uint16_t sequence)
{
	std::vector<std::uint8_t> icmp(8 + 32, 0);
	icmp[0] = type;
	put16(icmp, 4, echo_identifier);
	put16(icmp, 6, sequence);
	for (std::size_t index = 8; index < icmp.size(); ++index)
		icmp[index] = static_cast<std::uint8_t>('A' + index % 26);
	put16(icmp, 2, checksum_fold(checksum_add(0, icmp.data(), icmp.size())));
	return icmp;
}

std::vector<Packet> drain(Network &network)
{
	std::vector<Packet> packets;
	while (!network.frames().empty()) {
		if (auto packet = parse_ip(network.frames().front()))
			packets.push_back(std::move(*packet));
		network.frames().pop_front();
	}
	return packets;
}

/* Send one packet, then give SLiRP up to `passes` event-loop passes of its
 * own clock to answer; stop at the first pass that produced a packet. */
std::vector<Packet> send_and_wait(Network &network,
			     const std::vector<std::uint8_t> &frame,
			     int passes = 100)
{
	network.input(frame);
	std::vector<Packet> packets = drain(network);
	for (int pass = 0; pass < passes && packets.empty(); ++pass) {
		network.pump(20, 20);
		packets = drain(network);
	}
	return packets;
}

std::string text(const Address &address)
{
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", address[0], address[1],
		 address[2], address[3]);
	return buffer;
}

bool echo_matches(const Packet &packet, std::uint8_t type,
		  std::uint16_t sequence)
{
	const auto expected = echo_payload(type, sequence);
	if (packet.protocol != IPPROTO_ICMP || packet.payload.size() < 8 ||
	    packet.payload[0] != type)
		return false;
	/* The checksum differs with the type; compare identifier, sequence and
	 * the data. */
	return get16(packet.payload, 4) == echo_identifier &&
	       get16(packet.payload, 6) == sequence &&
	       packet.payload.size() == expected.size() &&
	       std::equal(packet.payload.begin() + 8, packet.payload.end(),
			  expected.begin() + 8);
}

int open_descriptors()
{
	int count = 0;
	for (int descriptor = 3; descriptor < 1024; ++descriptor)
		if (fcntl(descriptor, F_GETFD) != -1)
			count++;
	return count;
}

/* The first address of an interface that is up and not loopback, outside
 * the virtual network. A ping to it is answered by the host stack alone. */
std::optional<Address> host_address()
{
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return std::nullopt;
	std::optional<Address> found;
	for (const ifaddrs *entry = list; entry != nullptr && !found;
	     entry = entry->ifa_next) {
		if (entry->ifa_addr == nullptr ||
		    entry->ifa_addr->sa_family != AF_INET ||
		    (entry->ifa_flags & (IFF_UP | IFF_RUNNING)) !=
			    (IFF_UP | IFF_RUNNING) ||
		    (entry->ifa_flags & IFF_LOOPBACK) != 0)
			continue;
		const auto *inet =
			reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
		const auto *bytes =
			reinterpret_cast<const std::uint8_t *>(&inet->sin_addr);
		Address address{bytes[0], bytes[1], bytes[2], bytes[3]};
		if (address[0] == 10 && address[1] == 0 && address[2] == 2)
			continue;
		found = address;
	}
	freeifaddrs(list);
	return found;
}

void gateway_echo(Network &network)
{
	const auto packets = send_and_wait(
		network, ip_frame(guest_ip, gateway_ip, IPPROTO_ICMP, 64,
				  echo_payload(icmp_echo_request, 1)));
	if (packets.size() != 1 || !echo_matches(packets[0], icmp_echo_reply, 1) ||
	    packets[0].source != gateway_ip || packets[0].destination != guest_ip ||
	    packets[0].ttl != 255)
		fail("gateway echo",
		     "expected one echo reply from the gateway with TTL 255, got " +
			     std::to_string(packets.size()) + " packet(s)" +
			     (packets.empty() ? std::string{}
					      : ", TTL " +
							std::to_string(packets[0].ttl)));
}

void expired_echo(Network &network)
{
	/* RFC 5737 TEST-NET-1 again: nothing answers there, and a packet that
	 * expired at the SLiRP hop never leaves the host. */
	const Address destination{192, 0, 2, 1};
	const int before = open_descriptors();
	const auto frame = ip_frame(guest_ip, destination, IPPROTO_ICMP, 1,
				    echo_payload(icmp_echo_request, 2));
	const auto packets = send_and_wait(network, frame);
	if (packets.size() != 1 || packets[0].protocol != IPPROTO_ICMP ||
	    packets[0].source != gateway_ip ||
	    packets[0].destination != guest_ip || packets[0].payload.size() < 8 + 28 ||
	    packets[0].payload[0] != icmp_time_exceeded ||
	    packets[0].payload[1] != 0) {
		fail("expired echo",
		     "expected one ICMP time exceeded (in transit) from the "
		     "gateway, got " +
			     std::to_string(packets.size()) + " packet(s)");
		return;
	}
	/* The error quotes the expired packet: its IP header, then its first
	 * eight bytes. */
	const auto &quote = packets[0].payload;
	if (std::memcmp(&quote[8 + 16], destination.data(), 4) != 0 ||
	    quote[8 + 20] != icmp_echo_request ||
	    get16(quote, 8 + 20 + 6) != 2)
		fail("expired echo", "the error does not quote the expired echo");
	/* Nothing more may follow it, and no socket may be left behind. */
	network.pump(500, 20);
	if (!drain(network).empty())
		fail("expired echo", "extra packet after the time exceeded");
	if (open_descriptors() != before)
		fail("expired echo", "a host descriptor was left open");
}

void host_echo(Network &network)
{
	const auto host = host_address();
	if (!host) {
		/* No interface with an address: a host in this state has no
		 * stack to answer, so the case has nothing to measure. */
		printf("SKIP host echo: no non-loopback IPv4 address\n");
		return;
	}
	for (std::uint8_t ttl : {std::uint8_t{64}, std::uint8_t{5}}) {
		const std::uint16_t sequence = 10 + ttl;
		const auto packets = send_and_wait(
			network, ip_frame(guest_ip, *host, IPPROTO_ICMP, ttl,
					  echo_payload(icmp_echo_request, sequence)),
			150);
		const std::string label = "host echo (guest TTL " +
					  std::to_string(ttl) + ")";
		if (packets.size() != 1 ||
		    !echo_matches(packets[0], icmp_echo_reply, sequence) ||
		    packets[0].source != *host || packets[0].destination != guest_ip) {
			fail(label.c_str(),
			     "expected one echo reply from " + text(*host) +
				     ", got " + std::to_string(packets.size()) +
				     " packet(s)");
			continue;
		}
		/* The reply keeps the TTL it arrived with. SLiRP used to
		 * reset it to 255 whatever the host's stack sent. */
		if (packets[0].ttl == 255 || packets[0].ttl == 0)
			fail(label.c_str(),
			     "reply TTL " + std::to_string(packets[0].ttl) +
				     " is not the host stack's");
		else
			printf("%s: reply from %s with TTL %u\n", label.c_str(),
			       text(*host).c_str(),
			       static_cast<unsigned int>(packets[0].ttl));
	}
}

void martians(Network &network)
{
	struct Case {
		const char *label;
		Address source;
		Address destination;
	};
	const Case cases[] = {
		{"echo to loopback", guest_ip, {127, 0, 0, 1}},
		{"echo from loopback", {127, 0, 0, 1}, gateway_ip},
		{"echo to class E", guest_ip, {240, 0, 0, 1}},
		{"echo to the unspecified address", guest_ip, {0, 0, 0, 0}},
		{"echo from a multicast source", {224, 0, 0, 5}, gateway_ip},
	};
	for (const auto &item : cases) {
		const int before = open_descriptors();
		const auto packets = send_and_wait(
			network,
			ip_frame(item.source, item.destination, IPPROTO_ICMP, 64,
				 echo_payload(icmp_echo_request, 3)),
			10);
		if (!packets.empty())
			fail(item.label, "SLiRP answered a martian packet with " +
						 std::to_string(packets.size()) +
						 " packet(s)");
		if (open_descriptors() != before)
			fail(item.label, "a host descriptor was left open");
	}
}

/* A DHCP DISCOVER from 0.0.0.0 to 255.255.255.255, the way a guest with no
 * address sends it. */
void dhcp_discover(Network &network)
{
	std::vector<std::uint8_t> udp(8 + 548, 0);
	put16(udp, 0, 68);
	put16(udp, 2, 67);
	put16(udp, 4, static_cast<std::uint16_t>(udp.size()));
	std::uint8_t *bootp = udp.data() + 8;
	bootp[0] = 1; /* BOOTREQUEST */
	bootp[1] = 1; /* Ethernet */
	bootp[2] = 6;
	bootp[4] = 0x5a;
	bootp[5] = 0x17;
	bootp[6] = 0x4c;
	bootp[7] = 0x2d;
	std::copy(guest_mac.begin(), guest_mac.end(), bootp + 28);
	const std::uint8_t options[] = {0x63, 0x82, 0x53, 0x63, 53, 1, 1, 255};
	std::copy(std::begin(options), std::end(options), bootp + 236);
	const auto packets = send_and_wait(
		network, ip_frame({0, 0, 0, 0}, {255, 255, 255, 255}, IPPROTO_UDP,
				  64, udp));
	for (const auto &packet : packets) {
		if (packet.protocol != IPPROTO_UDP || packet.payload.size() < 8 + 240 ||
		    get16(packet.payload, 0) != 67 || get16(packet.payload, 2) != 68)
			continue;
		const Address offered{packet.payload[8 + 16], packet.payload[8 + 17],
				      packet.payload[8 + 18], packet.payload[8 + 19]};
		if (offered != guest_ip)
			fail("dhcp discover", "offered " + text(offered) + ", not " +
						      text(guest_ip));
		return;
	}
	fail("dhcp discover", "no DHCP offer for a DISCOVER from 0.0.0.0");
}

} // namespace

int main(void)
{
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fprintf(stderr, "SLiRP did not start or answer ARP\n");
		return 1;
	}

	gateway_echo(network);
	expired_echo(network);
	host_echo(network);
	martians(network);
	dhcp_discover(network);

	if (failures != 0)
		return 1;
	printf("PASS: the gateway answers an echo with a full TTL, an echo with "
	       "a TTL of one expires at SLiRP, a host reply keeps its TTL, "
	       "martian packets are dropped and DHCP from 0.0.0.0 is "
	       "answered\n");
	return 0;
}
