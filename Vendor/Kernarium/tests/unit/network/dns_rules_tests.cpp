// SPDX-License-Identifier: GPL-2.0-or-later
//
// The wire rules of the guest's DNS: what the provider reads from a guest
// frame, which frames it answers itself, and how it words an answer. Every
// rule is a pure function of bytes, so these tests need no socket, no guest
// and no system resolver.

#include "dns_frames.hpp"
#include "dns_message.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
namespace dns = uml::network::dns;
using Bytes = std::vector<std::uint8_t>;
using dns::FrameAction;
using dns::Ipv4Address;
using dns::MacAddress;

constexpr Ipv4Address name_server{10, 0, 2, 3};
constexpr Ipv4Address gateway{10, 0, 2, 2};
constexpr Ipv4Address guest{10, 0, 2, 15};
constexpr MacAddress guest_mac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
constexpr MacAddress slirp_mac{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

constexpr std::uint8_t protocol_icmp = 1;
constexpr std::uint8_t protocol_igmp = 2;
constexpr std::uint8_t protocol_tcp = 6;
constexpr std::uint8_t protocol_udp = 17;
constexpr std::uint8_t tcp_fin = 0x01;
constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_ack = 0x10;

std::uint16_t get16(const Bytes &bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) |
					  bytes[offset + 1]);
}

std::uint32_t get32(const Bytes &bytes, std::size_t offset)
{
	return (static_cast<std::uint32_t>(get16(bytes, offset)) << 16) |
		get16(bytes, offset + 2);
}

void put16(Bytes &bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
	put16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
	put16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

/* The Internet checksum of `bytes` after `sum`; zero over a valid packet. */
std::uint16_t checksum(const std::uint8_t *bytes, std::size_t size,
		       std::uint32_t sum = 0)
{
	for (std::size_t index = 0; index + 1 < size; index += 2)
		sum += static_cast<std::uint32_t>(bytes[index] << 8) |
			bytes[index + 1];
	if ((size & 1U) != 0)
		sum += static_cast<std::uint32_t>(bytes[size - 1]) << 8;
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return static_cast<std::uint16_t>(~sum);
}

std::uint16_t transport_checksum(const Ipv4Address &source,
				 const Ipv4Address &destination,
				 std::uint8_t protocol, const std::uint8_t *segment,
				 std::size_t size)
{
	std::uint32_t sum = protocol + static_cast<std::uint32_t>(size);
	for (std::size_t index = 0; index < 4; index += 2) {
		sum += static_cast<std::uint32_t>(source[index] << 8) |
			source[index + 1];
		sum += static_cast<std::uint32_t>(destination[index] << 8) |
			destination[index + 1];
	}
	return checksum(segment, size, sum);
}

/* An Ethernet frame from the guest. `options` is the number of 32-bit IP
 * option words; `fragment` is the IP flags and offset field. */
Bytes ip_frame(const Ipv4Address &destination, std::uint8_t protocol,
	       const Bytes &transport, std::uint16_t fragment = 0x4000,
	       std::size_t options = 0)
{
	const std::size_t header = 20 + 4 * options;
	Bytes frame(14 + header + transport.size(), 0);
	std::copy(slirp_mac.begin(), slirp_mac.end(), frame.begin());
	std::copy(guest_mac.begin(), guest_mac.end(), frame.begin() + 6);
	put16(frame, 12, 0x0800);
	frame[14] = static_cast<std::uint8_t>(0x40 | (5 + options));
	put16(frame, 16, static_cast<std::uint16_t>(header + transport.size()));
	put16(frame, 18, 0x1234);
	put16(frame, 20, fragment);
	frame[22] = 64;
	frame[23] = protocol;
	std::copy(guest.begin(), guest.end(), frame.begin() + 26);
	std::copy(destination.begin(), destination.end(), frame.begin() + 30);
	put16(frame, 24, checksum(&frame[14], header));
	std::copy(transport.begin(), transport.end(), frame.begin() + 14 + header);
	return frame;
}

Bytes udp_datagram(const Ipv4Address &destination, std::uint16_t source_port,
		   std::uint16_t destination_port, const Bytes &payload,
		   bool valid_checksum = true)
{
	Bytes udp(8 + payload.size(), 0);
	put16(udp, 0, source_port);
	put16(udp, 2, destination_port);
	put16(udp, 4, static_cast<std::uint16_t>(udp.size()));
	std::copy(payload.begin(), payload.end(), udp.begin() + 8);
	auto sum = transport_checksum(guest, destination, protocol_udp,
				      udp.data(), udp.size());
	if (sum == 0)
		sum = 0xffff;
	const auto wrong = static_cast<std::uint16_t>(sum ^ 0x5a5a);
	require(wrong != 0);
	put16(udp, 6, valid_checksum ? sum : wrong);
	return udp;
}

Bytes tcp_segment(const Ipv4Address &destination, std::uint16_t source_port,
		  std::uint16_t destination_port, std::uint32_t sequence,
		  std::uint32_t acknowledgement, std::uint8_t flags,
		  std::size_t payload = 0)
{
	Bytes tcp(20 + payload, 0);
	put16(tcp, 0, source_port);
	put16(tcp, 2, destination_port);
	put32(tcp, 4, sequence);
	put32(tcp, 8, acknowledgement);
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	put16(tcp, 14, 65535);
	put16(tcp, 16, transport_checksum(guest, destination, protocol_tcp,
					  tcp.data(), tcp.size()));
	return tcp;
}

Bytes query(std::uint16_t id, const std::string &name, std::uint16_t type,
	    bool edns = false, std::uint16_t payload = 1232,
	    std::uint8_t version = 0)
{
	Bytes message(12, 0);
	put16(message, 0, id);
	put16(message, 2, 0x0100);
	put16(message, 4, 1);
	put16(message, 10, edns ? 1 : 0);
	const auto encoded = dns::wire_name(name);
	require(encoded.has_value());
	message.insert(message.end(), encoded->begin(), encoded->end());
	message.resize(message.size() + 4);
	put16(message, message.size() - 4, type);
	put16(message, message.size() - 2, 1);
	if (edns) {
		Bytes opt(11, 0);
		opt[1] = 0;
		opt[2] = 41;
		put16(opt, 3, payload);
		opt[6] = version;
		message.insert(message.end(), opt.begin(), opt.end());
	}
	return message;
}

Bytes wire(const std::string &name)
{
	const auto encoded = dns::wire_name(name);
	require(encoded.has_value());
	return *encoded;
}

void messagesAreParsedAsTheGuestSentThem()
{
	const auto parsed = dns::parse_query(query(7, "Example.COM", 1, true));
	require(parsed.disposition == dns::Disposition::resolve);
	require(parsed.rcode == dns::rcode_noerror);
	require(parsed.query.id == 7);
	require(parsed.query.type == 1 && parsed.query.klass == 1);
	require(parsed.query.edns && parsed.query.edns_payload == 1232 &&
		parsed.query.edns_version == 0);
	/* The question keeps its case: a resolver using 0x20 checks it. */
	require(dns::presentation_name(parsed.query.name) == "Example.COM.");
	require(parsed.query.question.size() == parsed.query.name.size() + 4);

	const auto plain = dns::parse_query(query(8, "example.com", 28));
	require(plain.disposition == dns::Disposition::resolve);
	require(!plain.query.edns);
}

void udpLimitsFollowTheGuestsAdvertisement()
{
	require(dns::udp_limit(dns::parse_query(query(1, "a.example", 1)).query) ==
		dns::udp_classic_limit);
	const auto limit = [](std::uint16_t payload) {
		return dns::udp_limit(
			dns::parse_query(query(1, "a.example", 1, true, payload))
				.query);
	};
	require(limit(100) == dns::udp_classic_limit);
	require(limit(1232) == 1232);
	/* A larger advertisement still has to fit one 1500-byte frame. */
	require(limit(4096) == dns::udp_link_limit);
}

void messagesThatAreNotQueriesAreRefusedOrDropped()
{
	require(dns::parse_query(Bytes(11, 0)).disposition ==
		dns::Disposition::drop);
	auto response = query(10, "a.example", 1);
	response[2] |= 0x80;
	require(dns::parse_query(response).disposition ==
		dns::Disposition::drop);

	const auto refused = [](const Bytes &message, std::uint16_t rcode) {
		const auto parsed = dns::parse_query(message);
		return parsed.disposition == dns::Disposition::answer &&
			parsed.rcode == rcode && parsed.query.id == get16(message, 0);
	};
	auto two = query(8, "a.example", 1);
	put16(two, 4, 2);
	require(refused(two, dns::rcode_formerr));
	auto none = query(8, "a.example", 1);
	put16(none, 4, 0);
	require(refused(none, dns::rcode_formerr));
	auto update = query(9, "a.example", 1);
	update[2] |= 0x28; /* opcode 5, UPDATE */
	require(refused(update, dns::rcode_notimp));
	require(refused(query(11, "a.example", 1, true, 1232, 1),
			dns::rcode_badvers));
	require(refused(query(12, "a.example", 252), dns::rcode_notimp));
	require(refused(query(13, "a.example", 251), dns::rcode_notimp));
	require(refused(query(14, "a.example", 0), dns::rcode_formerr));
	require(refused(query(15, "a.example", 41), dns::rcode_formerr));
}

void malformedQuestionsAreFormatErrors()
{
	const auto formerr = [](const Bytes &message) {
		const auto parsed = dns::parse_query(message);
		return parsed.disposition == dns::Disposition::answer &&
			parsed.rcode == dns::rcode_formerr;
	};
	const auto good = query(1, "www.example.com", 1);
	/* Cut inside the type and class. */
	require(formerr(Bytes(good.begin(), good.end() - 2)));
	/* A compression pointer where the question's name starts. */
	auto pointer = good;
	pointer[12] = 0xc0;
	pointer[13] = 0x0c;
	require(formerr(pointer));
	/* A label longer than 63 bytes. */
	auto label = good;
	label[12] = 64;
	require(formerr(label));
	/* A name that runs past the message. */
	auto open = good;
	open[12 + 1 + 3] = 40;
	require(formerr(open));
}

void theAdditionalSectionIsWalkedForOneOptRecord()
{
	const auto formerr = [](const Bytes &message) {
		return dns::parse_query(message).rcode == dns::rcode_formerr;
	};
	auto two_opts = query(1, "a.example", 1, true);
	const Bytes opt(two_opts.end() - 11, two_opts.end());
	two_opts.insert(two_opts.end(), opt.begin(), opt.end());
	put16(two_opts, 10, 2);
	require(formerr(two_opts));
	/* An OPT record in the answer section is not one. */
	auto misplaced = query(1, "a.example", 1, true);
	put16(misplaced, 6, 1);
	put16(misplaced, 10, 0);
	require(formerr(misplaced));
	/* An OPT record with a name is not one either. */
	auto named = query(1, "a.example", 1);
	const Bytes pointed{0xc0, 12, 0, 41, 0x04, 0xd0, 0, 0, 0, 0, 0, 0};
	named.insert(named.end(), pointed.begin(), pointed.end());
	put16(named, 10, 1);
	require(formerr(named));
	/* A record that claims more data than the message holds. */
	auto overrun = query(1, "a.example", 1, true);
	put16(overrun, overrun.size() - 2, 8);
	require(formerr(overrun));
	/* Counts that promise records the message does not carry. */
	auto missing = query(1, "a.example", 1);
	put16(missing, 10, 3);
	require(formerr(missing));
}

void namesRoundTripThroughThePresentationForm()
{
	const auto escaped = dns::wire_name("a\\.b.c\\032d\\\\.");
	require(escaped.has_value() && escaped->size() == 10);
	require((*escaped)[0] == 3 && (*escaped)[2] == '.');
	require(dns::presentation_name(*escaped) == "a\\.b.c\\032d\\\\.");
	require(dns::wire_name("").value() == Bytes{0});
	require(dns::wire_name(".").value() == Bytes{0});
	require(dns::presentation_name(Bytes{0}) == ".");
	/* Bytes a C string cannot carry come out as decimal escapes. */
	require(dns::presentation_name(Bytes{2, 0, 0xff, 0}) == "\\000\\255.");
	require(dns::wire_name("\\000\\255").value() == (Bytes{2, 0, 0xff, 0}));
}

void malformedPresentationNamesAreRefused()
{
	require(!dns::wire_name("a..b"));
	require(!dns::wire_name(".a"));
	require(!dns::wire_name("a\\"));
	require(!dns::wire_name("a\\256"));
	require(!dns::wire_name(std::string(64, 'a') + ".example"));
	require(dns::wire_name(std::string(63, 'a') + ".example").has_value());
	/* 127 two-byte labels and the root are 255 bytes, the most a name has. */
	std::string longest;
	for (int index = 0; index < 127; ++index)
		longest += "a.";
	require(dns::wire_name(longest).value().size() == 255);
	require(!dns::wire_name(longest + "a."));
}

void namesCompareWithoutCase()
{
	require(dns::same_name(wire("WWW.Example."), wire("www.example")));
	require(!dns::same_name(wire("www.example"), wire("www.example.com")));
	require(!dns::same_name(wire("a.example"), wire("b.example")));
	/* Only ASCII letters fold. */
	require(!dns::same_name(Bytes{1, 0xc1, 0}, Bytes{1, 0xe1, 0}));
}

void aCnameTargetIsOneWholeUncompressedName()
{
	require(dns::cname_target(wire("target.example")).value() ==
		wire("target.example"));
	auto trailing = wire("target.example");
	trailing.push_back(0);
	require(!dns::cname_target(trailing));
	require(!dns::cname_target(Bytes{0xc0, 0x0c}));
	require(!dns::cname_target(Bytes{5, 'a', 'b'}));
	require(!dns::cname_target(Bytes{}));
}

struct Header final {
	std::uint16_t id{};
	std::uint16_t flags{};
	std::uint16_t questions{};
	std::uint16_t answers{};
	std::uint16_t authorities{};
	std::uint16_t additionals{};
};

Header header(const Bytes &message)
{
	require(message.size() >= 12);
	return {get16(message, 0), get16(message, 2), get16(message, 4),
		get16(message, 6), get16(message, 8), get16(message, 10)};
}

dns::Record address_record(const std::string &owner, std::uint8_t last)
{
	return {wire(owner), 1, 1, 300, {192, 0, 2, last}};
}

void answersEchoTheQuestionAndWordTheHeader()
{
	auto message = query(0x4242, "Example.COM", 1);
	message[3] |= 0x20; /* AD, which this server never claims */
	message[3] |= 0x10; /* CD, which it passes on */
	const auto parsed = dns::parse_query(message);
	const std::vector<dns::Record> records{address_record("example.com", 7)};
	const auto reply = dns::build_response(parsed.query, dns::rcode_noerror,
					       records, 512);
	const auto head = header(reply);
	require(head.id == 0x4242);
	/* QR, RD and RA, with CD copied and AA and AD clear. */
	require(head.flags == (0x8000 | 0x0100 | 0x0080 | 0x0010));
	require(head.questions == 1 && head.answers == 1 &&
		head.authorities == 0 && head.additionals == 0);
	require(std::equal(parsed.query.question.begin(),
			   parsed.query.question.end(), reply.begin() + 12));
	/* The answer's owner points at the question's name, case and all. */
	const std::size_t answer = 12 + parsed.query.question.size();
	require(reply[answer] == 0xc0 && reply[answer + 1] == 12);
	require(get16(reply, answer + 2) == 1 && get16(reply, answer + 4) == 1);
	require(get32(reply, answer + 6) == 300 && get16(reply, answer + 10) == 4);
	require(reply.size() == answer + 12 + 4 && reply[answer + 15] == 7);
}

void anOwnerThatIsNotTheQuestionIsWrittenOut()
{
	const auto parsed = dns::parse_query(query(1, "www.example.com", 1));
	const std::vector<dns::Record> records{
		{wire("www.example.com"), 5, 1, 60, wire("edge.example.net")},
		address_record("edge.example.net", 9)};
	const auto reply = dns::build_response(parsed.query, dns::rcode_noerror,
					       records, 512);
	require(header(reply).answers == 2);
	const std::size_t first = 12 + parsed.query.question.size();
	require(reply[first] == 0xc0);
	const std::size_t second = first + 2 + 10 + records[0].data.size();
	const auto owner = wire("edge.example.net");
	require(std::equal(owner.begin(), owner.end(), reply.begin() + second));
}

void optRecordsAreEchoedOnlyForEdnsQueries()
{
	const auto plain = dns::parse_query(query(1, "a.example", 1));
	require(header(dns::build_response(plain.query, 0, {}, 512)).additionals ==
		0);

	const auto edns = dns::parse_query(query(1, "a.example", 1, true, 4096));
	const auto reply = dns::build_response(edns.query, 0, {}, 512);
	require(header(reply).additionals == 1);
	const std::size_t opt = reply.size() - 11;
	/* Root owner, type OPT, our own payload size, no options. */
	require(reply[opt] == 0 && get16(reply, opt + 1) == 41 &&
		get16(reply, opt + 3) == 1232 && get16(reply, opt + 9) == 0);

	/* BADVERS needs the high bits of its code in the OPT record. */
	const auto bad = dns::build_response(edns.query, dns::rcode_badvers, {},
					     512);
	require((get16(bad, 2) & 0x0f) == 0);
	require(bad[bad.size() - 11 + 5] == 1);
}

void anAnswerTooLongForUdpKeepsOnlyItsQuestion()
{
	const auto parsed = dns::parse_query(query(13, "example.com", 16));
	std::vector<dns::Record> records;
	for (int index = 0; index < 40; ++index)
		records.push_back({wire("example.com"), 16, 1, 60,
				   Bytes(40, 'x')});
	const auto udp = dns::build_response(
		parsed.query, dns::rcode_noerror, records,
		dns::udp_limit(parsed.query));
	const auto head = header(udp);
	require((head.flags & 0x0200) != 0 && head.answers == 0 &&
		head.questions == 1);
	require(udp.size() == 12 + parsed.query.question.size());
	/* The same answer fits a TCP message whole. */
	const auto tcp = dns::build_response(parsed.query, dns::rcode_noerror,
					     records, dns::tcp_limit);
	require((header(tcp).flags & 0x0200) == 0 && header(tcp).answers == 40);
}

void aNegativeAnswerKeepsItsCode()
{
	const auto parsed = dns::parse_query(query(1, "a.example", 1));
	const auto reply = dns::build_response(parsed.query, dns::rcode_servfail,
					       {}, 512);
	require((header(reply).flags & 0x0f) == dns::rcode_servfail);
	require(header(reply).answers == 0);
}

void anAnswerNoTcpMessageCanCarryIsServerFailure()
{
	const auto parsed = dns::parse_query(query(1, "a.example", 16));
	const std::vector<dns::Record> huge{
		{wire("a.example"), 16, 1, 60, Bytes(60000, 'x')},
		{wire("a.example"), 16, 1, 60, Bytes(60000, 'y')}};
	const auto reply = dns::build_response(parsed.query, dns::rcode_noerror,
					       huge, dns::tcp_limit);
	require((header(reply).flags & 0x0f) == dns::rcode_servfail);
	require(header(reply).answers == 0 && (header(reply).flags & 0x0200) == 0);
}

dns::Classified classify(const Bytes &frame, bool streams = true)
{
	return dns::classify_guest_frame(
		frame, {name_server, gateway, streams, streams});
}

void aQueryToTheNameServerOrTheGatewayIsAnswered()
{
	const Bytes payload = query(5, "example.com", 1);
	for (const auto &server : {name_server, gateway}) {
		const auto frame = ip_frame(
			server, protocol_udp, udp_datagram(server, 40000, 53, payload));
		const auto classified = classify(frame);
		require(classified.action == FrameAction::query);
		require(classified.endpoints.guest_address == guest);
		require(classified.endpoints.server_address == server);
		require(classified.endpoints.guest_mac == guest_mac);
		require(classified.endpoints.guest_port == 40000 &&
			classified.endpoints.server_port == 53);
		require(Bytes(classified.payload.begin(), classified.payload.end()) ==
			payload);
	}
}

void aDatagramWithoutAChecksumIsStillAnswered()
{
	auto udp = udp_datagram(name_server, 40000, 53, query(5, "a.example", 1));
	put16(udp, 6, 0);
	require(classify(ip_frame(name_server, protocol_udp, udp)).action ==
		FrameAction::query);
}

void aDatagramWithABadChecksumIsDropped()
{
	const auto udp = udp_datagram(name_server, 40000, 53,
				      query(5, "a.example", 1), false);
	require(classify(ip_frame(name_server, protocol_udp, udp)).action ==
		FrameAction::drop);
	require(classify(ip_frame(gateway, protocol_udp,
				  udp_datagram(gateway, 40000, 53,
					       query(5, "a.example", 1), false)))
			.action == FrameAction::drop);
}

void theNameServerServesNothingButDns()
{
	const Bytes ntp(48, 0);
	require(classify(ip_frame(name_server, protocol_udp,
				  udp_datagram(name_server, 40000, 123, ntp)))
			.action == FrameAction::drop);
	/* The gateway keeps its other services. */
	require(classify(ip_frame(gateway, protocol_udp,
				  udp_datagram(gateway, 40000, 123, ntp)))
			.action == FrameAction::pass);
	require(classify(ip_frame(name_server, protocol_igmp, Bytes(8, 0)))
			.action == FrameAction::drop);
	require(classify(ip_frame(gateway, protocol_igmp, Bytes(8, 0))).action ==
		FrameAction::pass);
	/* ICMP echo to either is SLiRP's to answer. */
	require(classify(ip_frame(name_server, protocol_icmp, Bytes(8, 0))).action ==
		FrameAction::pass);
}

void fragmentsOfDnsNeverReachSlirp()
{
	const auto udp = udp_datagram(name_server, 40000, 53, query(5, "a.example", 1));
	/* More-fragments set, and a later fragment. */
	require(classify(ip_frame(name_server, protocol_udp, udp, 0x2000)).action ==
		FrameAction::drop);
	require(classify(ip_frame(name_server, protocol_udp, udp, 0x0010)).action ==
		FrameAction::drop);
	/* The gateway's first fragment shows port 53, a later one shows nothing. */
	require(classify(ip_frame(gateway, protocol_udp,
				  udp_datagram(gateway, 40000, 53,
					       query(5, "a.example", 1)),
				  0x2000))
			.action == FrameAction::drop);
	require(classify(ip_frame(gateway, protocol_udp, Bytes(64, 0), 0x0010))
			.action == FrameAction::pass);
	require(classify(ip_frame(gateway, protocol_udp,
				  udp_datagram(gateway, 40000, 123, Bytes(8, 0)),
				  0x2000))
			.action == FrameAction::pass);
}

void multicastNeverLeavesTheVirtualLink()
{
	for (const Ipv4Address group : {Ipv4Address{224, 0, 0, 251},
					Ipv4Address{239, 255, 255, 250},
					Ipv4Address{224, 0, 0, 22}}) {
		require(classify(ip_frame(group, protocol_udp,
					  udp_datagram(group, 5353, 5353,
						       query(0, "adb.example", 255))))
				.action == FrameAction::drop);
		require(classify(ip_frame(group, protocol_igmp, Bytes(8, 0))).action ==
			FrameAction::drop);
	}
	/* Class E and the limited broadcast are SLiRP's to judge. */
	require(classify(ip_frame(Ipv4Address{240, 0, 0, 1}, protocol_udp,
				  Bytes(8, 0)))
			.action == FrameAction::pass);
	require(classify(ip_frame(Ipv4Address{255, 255, 255, 255}, protocol_udp,
				  udp_datagram(Ipv4Address{255, 255, 255, 255}, 68, 67,
					       Bytes(8, 0))))
			.action == FrameAction::pass);
}

void framesThatAreNotIpv4AreLeftToSlirp()
{
	Bytes arp(42, 0);
	put16(arp, 12, 0x0806);
	require(classify(arp).action == FrameAction::pass);
	Bytes v6(60, 0);
	put16(v6, 12, 0x86dd);
	require(classify(v6).action == FrameAction::pass);
	require(classify(Bytes(20, 0)).action == FrameAction::pass);
	auto version = ip_frame(name_server, protocol_udp, Bytes(8, 0));
	version[14] = 0x65;
	require(classify(version).action == FrameAction::pass);
}

void aMalformedPacketIsDroppedAtTheNameServerAndPassedAtTheGateway()
{
	const auto udp = udp_datagram(name_server, 40000, 53, query(5, "a.example", 1));
	auto short_header = ip_frame(name_server, protocol_udp, udp);
	short_header[14] = 0x44; /* a header of 16 bytes */
	require(classify(short_header).action == FrameAction::drop);
	auto overrun = ip_frame(name_server, protocol_udp, udp);
	put16(overrun, 16, 2000);
	require(classify(overrun).action == FrameAction::drop);
	auto cut = ip_frame(name_server, protocol_udp, Bytes(6, 0));
	require(classify(cut).action == FrameAction::drop);
	auto lie = ip_frame(name_server, protocol_udp, udp);
	put16(lie, 14 + 20 + 4, 4); /* a UDP length below its header */
	require(classify(lie).action == FrameAction::drop);

	auto gateway_overrun = ip_frame(gateway, protocol_udp, Bytes(12, 0));
	put16(gateway_overrun, 16, 2000);
	require(classify(gateway_overrun).action == FrameAction::pass);
}

void aBadHeaderChecksumIsDroppedAtTheNameServer()
{
	const auto payload = query(5, "a.example", 1);
	for (const auto &server : {name_server, gateway}) {
		auto frame = ip_frame(server, protocol_udp,
				      udp_datagram(server, 40000, 53, payload));
		require(classify(frame).action == FrameAction::query);
		frame[24] ^= 0x01;
		/* SLiRP drops what the gateway is sent; it must not see the
		 * name server's. */
		require(classify(frame).action ==
			(server == name_server ? FrameAction::drop :
						 FrameAction::pass));
	}
	auto reset = ip_frame(name_server, protocol_tcp,
			      tcp_segment(name_server, 41000, 853, 1, 0, tcp_syn));
	reset[25] ^= 0x80;
	require(classify(reset).action == FrameAction::drop);
}

void ipOptionsAreSkippedToFindTheDatagram()
{
	const Bytes payload = query(5, "a.example", 1);
	const auto frame = ip_frame(name_server, protocol_udp,
				    udp_datagram(name_server, 40000, 53, payload),
				    0x4000, 2);
	const auto classified = classify(frame);
	require(classified.action == FrameAction::query);
	require(Bytes(classified.payload.begin(), classified.payload.end()) ==
		payload);
}

void aConnectionToAnotherPortOfTheNameServerIsReset()
{
	const auto syn = classify(ip_frame(
		name_server, protocol_tcp,
		tcp_segment(name_server, 41000, 853, 5000, 0, tcp_syn)));
	require(syn.action == FrameAction::reset);
	require(syn.endpoints.server_port == 853 &&
		syn.endpoints.guest_port == 41000);
	require(syn.sequence == 5000 && syn.segment_length == 1);

	/* RFC 9293 3.10.7.1: the reset to a segment with no ACK acknowledges
	 * what the segment occupied. */
	const auto reset = dns::tcp_reset_frame(syn, 7);
	require(reset.size() == 14 + 20 + 20);
	require(checksum(&reset[14], 20) == 0);
	require(transport_checksum(name_server, guest, protocol_tcp, &reset[34],
				   20) == 0);
	require(std::equal(guest_mac.begin(), guest_mac.end(), reset.begin()));
	require(std::equal(slirp_mac.begin(), slirp_mac.end(), reset.begin() + 6));
	require(std::equal(name_server.begin(), name_server.end(),
			   reset.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), reset.begin() + 30));
	require(get16(reset, 34) == 853 && get16(reset, 36) == 41000);
	require(get32(reset, 38) == 0 && get32(reset, 42) == 5001);
	require(reset[47] == (tcp_rst | tcp_ack));
}

void aResetAnswersASegmentThatAcknowledgesWithItsAcknowledgement()
{
	const auto segment = classify(ip_frame(
		name_server, protocol_tcp,
		tcp_segment(name_server, 41000, 853, 5001, 9000, tcp_ack, 12)));
	require(segment.action == FrameAction::reset);
	require(segment.segment_length == 12);
	const auto reset = dns::tcp_reset_frame(segment, 8);
	require(get32(reset, 38) == 9000 && reset[47] == tcp_rst);
	require(transport_checksum(name_server, guest, protocol_tcp, &reset[34],
				   20) == 0);

	const auto fin = classify(ip_frame(
		name_server, protocol_tcp,
		tcp_segment(name_server, 41000, 853, 5001, 0, tcp_fin, 0)));
	require(fin.segment_length == 1);
}

void aResetIsNeverAnswered()
{
	const auto segment = classify(ip_frame(
		name_server, protocol_tcp,
		tcp_segment(name_server, 41000, 853, 5001, 9000, tcp_rst | tcp_ack)));
	require(segment.action == FrameAction::reset);
	require(dns::tcp_reset_frame(segment, 1).empty());
}

void dnsOverTcpIsLeftToSlirpsGuestForward()
{
	for (const auto &server : {name_server, gateway}) {
		require(classify(ip_frame(
				server, protocol_tcp,
				tcp_segment(server, 41000, 53, 5000, 0, tcp_syn)))
				.action == FrameAction::pass);
	}
	/* The gateway's other ports are SLiRP's, the name server's are not. */
	require(classify(ip_frame(gateway, protocol_tcp,
				  tcp_segment(gateway, 41000, 80, 5000, 0, tcp_syn)))
			.action == FrameAction::pass);
}

void dnsOverTcpWithoutAForwardIsReset()
{
	/* SLiRP, with its own DNS proxy off, would connect to the literal
	 * address on the host's network. */
	for (const auto &server : {name_server, gateway}) {
		const auto syn = classify(
			ip_frame(server, protocol_tcp,
				 tcp_segment(server, 41000, 53, 5000, 0, tcp_syn)),
			false);
		require(syn.action == FrameAction::reset);
		require(syn.endpoints.server_port == 53 &&
			syn.endpoints.server_address == server);
		require(!dns::tcp_reset_frame(syn, 1).empty());
	}
	/* The gateway's other ports stay SLiRP's. */
	require(classify(ip_frame(gateway, protocol_tcp,
				  tcp_segment(gateway, 41000, 80, 5000, 0, tcp_syn)),
			 false)
			.action == FrameAction::pass);
	/* Each address has its own forward. */
	const dns::Servers one{name_server, gateway, true, false};
	require(dns::classify_guest_frame(
			ip_frame(name_server, protocol_tcp,
				 tcp_segment(name_server, 41000, 53, 5000, 0, tcp_syn)),
			one)
			.action == FrameAction::pass);
	require(dns::classify_guest_frame(
			ip_frame(gateway, protocol_tcp,
				 tcp_segment(gateway, 41000, 53, 5000, 0, tcp_syn)),
			one)
			.action == FrameAction::reset);
}

void aReplyIsAWellFormedUdpFrame()
{
	const auto frame = ip_frame(name_server, protocol_udp,
				    udp_datagram(name_server, 40000, 53,
						 query(5, "a.example", 1)));
	const auto classified = classify(frame);
	require(classified.action == FrameAction::query);
	const Bytes payload = query(5, "a.example", 1);
	const auto reply = dns::udp_reply_frame(classified.endpoints, payload, 99);
	require(reply.size() == 14 + 20 + 8 + payload.size());
	require(get16(reply, 12) == 0x0800 && reply[14] == 0x45);
	require(checksum(&reply[14], 20) == 0);
	require(get16(reply, 18) == 99 && (get16(reply, 20) & 0x1fff) == 0);
	require(std::equal(guest_mac.begin(), guest_mac.end(), reply.begin()));
	require(std::equal(slirp_mac.begin(), slirp_mac.end(), reply.begin() + 6));
	require(std::equal(name_server.begin(), name_server.end(),
			   reply.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), reply.begin() + 30));
	require(get16(reply, 34) == 53 && get16(reply, 36) == 40000);
	require(transport_checksum(name_server, guest, protocol_udp, &reply[34],
				   8 + payload.size()) == 0);
	require(Bytes(reply.begin() + 42, reply.end()) == payload);
}

} // namespace

int main()
{
	messagesAreParsedAsTheGuestSentThem();
	udpLimitsFollowTheGuestsAdvertisement();
	messagesThatAreNotQueriesAreRefusedOrDropped();
	malformedQuestionsAreFormatErrors();
	theAdditionalSectionIsWalkedForOneOptRecord();
	namesRoundTripThroughThePresentationForm();
	malformedPresentationNamesAreRefused();
	namesCompareWithoutCase();
	aCnameTargetIsOneWholeUncompressedName();
	answersEchoTheQuestionAndWordTheHeader();
	anOwnerThatIsNotTheQuestionIsWrittenOut();
	optRecordsAreEchoedOnlyForEdnsQueries();
	anAnswerTooLongForUdpKeepsOnlyItsQuestion();
	aNegativeAnswerKeepsItsCode();
	anAnswerNoTcpMessageCanCarryIsServerFailure();
	aQueryToTheNameServerOrTheGatewayIsAnswered();
	aDatagramWithoutAChecksumIsStillAnswered();
	aDatagramWithABadChecksumIsDropped();
	theNameServerServesNothingButDns();
	fragmentsOfDnsNeverReachSlirp();
	multicastNeverLeavesTheVirtualLink();
	framesThatAreNotIpv4AreLeftToSlirp();
	aMalformedPacketIsDroppedAtTheNameServerAndPassedAtTheGateway();
	aBadHeaderChecksumIsDroppedAtTheNameServer();
	ipOptionsAreSkippedToFindTheDatagram();
	aConnectionToAnotherPortOfTheNameServerIsReset();
	aResetAnswersASegmentThatAcknowledgesWithItsAcknowledgement();
	aResetIsNeverAnswered();
	dnsOverTcpIsLeftToSlirpsGuestForward();
	dnsOverTcpWithoutAForwardIsReset();
	aReplyIsAWellFormedUdpFrame();
	return EXIT_SUCCESS;
}
