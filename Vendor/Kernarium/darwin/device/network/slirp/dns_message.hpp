// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The DNS wire format the guest's stub resolver speaks to the virtual name
 * server, reduced to what a forwarding stub needs: read one question, write
 * one answer. Nothing here touches a socket, so every rule is a pure function
 * of its bytes.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::network::dns {

inline constexpr std::uint16_t type_cname = 5;
inline constexpr std::uint16_t type_opt = 41;
inline constexpr std::uint16_t type_ixfr = 251;
inline constexpr std::uint16_t type_axfr = 252;
inline constexpr std::uint16_t type_any = 255;

inline constexpr std::uint8_t rcode_noerror = 0;
inline constexpr std::uint8_t rcode_formerr = 1;
inline constexpr std::uint8_t rcode_servfail = 2;
inline constexpr std::uint8_t rcode_nxdomain = 3;
inline constexpr std::uint8_t rcode_notimp = 4;
inline constexpr std::uint8_t rcode_refused = 5;
/* Extended (EDNS) code 16: the high 8 bits live in the OPT record. */
inline constexpr std::uint16_t rcode_badvers = 16;

/* The largest UDP answer that leaves the 1500-byte link in one frame. A
 * longer one is truncated and the guest asks again over TCP. */
inline constexpr std::size_t udp_link_limit = 1500 - 20 - 8;
/* What a query without EDNS may receive (RFC 1035 4.2.1). */
inline constexpr std::size_t udp_classic_limit = 512;
/* A DNS-over-TCP message carries a 16-bit length (RFC 1035 4.2.2). */
inline constexpr std::size_t tcp_limit = 65535;

struct Query final {
	std::uint16_t id{};
	/* The header's second 16 bits as the guest sent them. */
	std::uint16_t flags{};
	/* The question exactly as sent, case included (RFC 5452 0x20 checks). */
	std::vector<std::uint8_t> question;
	/* The question name alone, uncompressed wire form. */
	std::vector<std::uint8_t> name;
	std::uint16_t type{};
	std::uint16_t klass{};
	bool edns{};
	std::uint8_t edns_version{};
	std::uint16_t edns_payload{};
};

/* What to do with a datagram a guest sent to the name server. */
enum class Disposition {
	resolve, /* a standard query to look up */
	answer,  /* answer at once with `rcode` and no records */
	drop,    /* not a query: never answered */
};

struct Parsed final {
	Disposition disposition{Disposition::drop};
	std::uint16_t rcode{};
	Query query;
};

[[nodiscard]] Parsed parse_query(std::span<const std::uint8_t> message);

struct Record final {
	std::vector<std::uint8_t> owner; /* uncompressed wire name */
	std::uint16_t type{};
	std::uint16_t klass{};
	std::uint32_t ttl{};
	std::vector<std::uint8_t> data; /* RDATA as the resolver gave it */
};

/*
 * The answer to `query`. A message longer than `limit` keeps only its
 * question and the TC bit, so the guest asks again over TCP; one longer than
 * a TCP message can carry becomes SERVFAIL.
 */
[[nodiscard]] std::vector<std::uint8_t> build_response(
	const Query &query, std::uint16_t rcode,
	std::span<const Record> answers, std::size_t limit);

/* The largest answer a UDP query may receive on this link. */
[[nodiscard]] std::size_t udp_limit(const Query &query) noexcept;

/* A dns_sd full name (RFC 1035 presentation with \DDD and \X escapes) as an
 * uncompressed wire name, or nothing when it is malformed. */
[[nodiscard]] std::optional<std::vector<std::uint8_t>> wire_name(
	std::string_view presentation);

/* An uncompressed wire name in the presentation form dns_sd parses. */
[[nodiscard]] std::string presentation_name(
	std::span<const std::uint8_t> wire);

/* ASCII case-insensitive comparison of two uncompressed wire names. */
[[nodiscard]] bool same_name(std::span<const std::uint8_t> left,
			     std::span<const std::uint8_t> right) noexcept;

/* The target of a CNAME record's RDATA, which dns_sd passes uncompressed. */
[[nodiscard]] std::optional<std::vector<std::uint8_t>> cname_target(
	std::span<const std::uint8_t> data);

} // namespace uml::network::dns
