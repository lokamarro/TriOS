/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A SLiRP instance driven directly, with a guest the probe writes by hand and
 * a clock the probe advances, so a TCP timer that runs for minutes on a real
 * clock runs in a few thousand event-loop passes. Host sockets stay real.
 */
#pragma once

#include <libslirp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <vector>

namespace slirp_harness {

inline constexpr std::array<std::uint8_t, 4> guest_ip{10, 0, 2, 15};
inline constexpr std::array<std::uint8_t, 4> gateway_ip{10, 0, 2, 2};
inline constexpr std::array<std::uint8_t, 6> guest_mac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

inline constexpr std::uint8_t tcp_fin = 0x01;
inline constexpr std::uint8_t tcp_syn = 0x02;
inline constexpr std::uint8_t tcp_rst = 0x04;
inline constexpr std::uint8_t tcp_psh = 0x08;
inline constexpr std::uint8_t tcp_ack = 0x10;

/* One TCP segment SLiRP sent to the guest. */
struct Segment {
	std::array<std::uint8_t, 4> source{};
	std::array<std::uint8_t, 4> destination{};
	std::uint16_t source_port{};
	std::uint16_t destination_port{};
	std::uint32_t seq{};
	std::uint32_t ack{};
	std::uint8_t flags{};
	std::uint16_t window{};
	std::size_t payload{};
};

inline void put16(std::vector<std::uint8_t> &frame, std::size_t offset,
		  std::uint16_t value)
{
	frame[offset] = static_cast<std::uint8_t>(value >> 8);
	frame[offset + 1] = static_cast<std::uint8_t>(value);
}

inline void put32(std::vector<std::uint8_t> &frame, std::size_t offset,
		  std::uint32_t value)
{
	put16(frame, offset, static_cast<std::uint16_t>(value >> 16));
	put16(frame, offset + 2, static_cast<std::uint16_t>(value));
}

inline std::uint16_t get16(const std::vector<std::uint8_t> &frame,
			   std::size_t offset)
{
	return static_cast<std::uint16_t>((frame[offset] << 8) |
					  frame[offset + 1]);
}

inline std::uint32_t get32(const std::vector<std::uint8_t> &frame,
			   std::size_t offset)
{
	return (static_cast<std::uint32_t>(get16(frame, offset)) << 16) |
		get16(frame, offset + 2);
}

inline std::uint32_t checksum_add(std::uint32_t sum,
				  const std::uint8_t *bytes,
				  std::size_t length)
{
	for (std::size_t index = 0; index + 1 < length; index += 2)
		sum += static_cast<std::uint32_t>((bytes[index] << 8) |
						  bytes[index + 1]);
	if ((length & 1) != 0)
		sum += static_cast<std::uint32_t>(bytes[length - 1] << 8);
	return sum;
}

inline std::uint16_t checksum_fold(std::uint32_t sum)
{
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return static_cast<std::uint16_t>(~sum);
}

inline std::optional<Segment> parse_tcp(const std::vector<std::uint8_t> &frame)
{
	if (frame.size() < 34 || get16(frame, 12) != 0x0800 ||
	    frame[23] != IPPROTO_TCP)
		return std::nullopt;
	const std::size_t ip_header = (frame[14] & 0x0f) * 4u;
	const std::size_t tcp = 14 + ip_header;
	if (frame.size() < tcp + 20)
		return std::nullopt;
	const std::size_t tcp_header = (frame[tcp + 12] >> 4) * 4u;
	const std::size_t ip_length = get16(frame, 16);
	if (ip_length < ip_header + tcp_header ||
	    frame.size() < 14 + ip_length)
		return std::nullopt;
	Segment segment;
	for (std::size_t index = 0; index < 4; ++index) {
		segment.source[index] = frame[26 + index];
		segment.destination[index] = frame[30 + index];
	}
	segment.source_port = get16(frame, tcp);
	segment.destination_port = get16(frame, tcp + 2);
	segment.seq = get32(frame, tcp + 4);
	segment.ack = get32(frame, tcp + 8);
	segment.flags = frame[tcp + 13];
	segment.window = get16(frame, tcp + 14);
	segment.payload = ip_length - ip_header - tcp_header;
	return segment;
}

/* A guest TCP segment. A SYN carries an MSS option of 1460. */
inline std::vector<std::uint8_t> tcp_frame(
	const std::array<std::uint8_t, 4> &source,
	const std::array<std::uint8_t, 4> &destination,
	std::uint16_t source_port, std::uint16_t destination_port,
	std::uint32_t seq, std::uint32_t ack, std::uint8_t flags,
	std::uint16_t window, std::size_t payload = 0)
{
	const std::size_t options = (flags & tcp_syn) != 0 ? 4 : 0;
	const std::size_t tcp_length = 20 + options + payload;
	std::vector<std::uint8_t> frame(14 + 20 + tcp_length, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + tcp_length));
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = IPPROTO_TCP;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = source[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));

	const std::size_t tcp = 34;
	put16(frame, tcp, source_port);
	put16(frame, tcp + 2, destination_port);
	put32(frame, tcp + 4, seq);
	put32(frame, tcp + 8, ack);
	frame[tcp + 12] = static_cast<std::uint8_t>(((20 + options) / 4) << 4);
	frame[tcp + 13] = flags;
	put16(frame, tcp + 14, window);
	if (options != 0) {
		frame[tcp + 20] = 2;
		frame[tcp + 21] = 4;
		put16(frame, tcp + 22, 1460);
	}
	for (std::size_t index = 0; index < payload; ++index)
		frame[tcp + 20 + options + index] =
			static_cast<std::uint8_t>('a' + index % 26);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_TCP;
	sum += static_cast<std::uint32_t>(tcp_length);
	sum = checksum_add(sum, &frame[tcp], tcp_length);
	put16(frame, tcp + 16, checksum_fold(sum));
	return frame;
}

class Network {
public:
	Network()
	{
		SlirpConfig config{};
		config.version = 6;
		config.in_enabled = true;
		(void)inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
		(void)inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
		(void)inet_pton(AF_INET, "10.0.2.2", &config.vhost);
		(void)inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
		(void)inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
		config.if_mtu = 1500;
		config.if_mru = 1500;
		slirp_ = slirp_new(&config, &callbacks(), this);
	}

	~Network()
	{
		if (slirp_ != nullptr)
			slirp_cleanup(slirp_);
	}

	Network(const Network &) = delete;
	Network &operator=(const Network &) = delete;

	[[nodiscard]] Slirp *slirp() const noexcept { return slirp_; }
	[[nodiscard]] std::int64_t now_ms() const noexcept
	{
		return now_ns_ / 1'000'000;
	}

	void input(const std::vector<std::uint8_t> &frame)
	{
		slirp_input(slirp_, frame.data(), static_cast<int>(frame.size()));
	}

	/* One event-loop pass after the clock moves by advance_ms. SLiRP runs
	 * its 500 ms TCP timer at most once per pass. */
	void pump(std::int64_t advance_ms, int wait_ms = 0)
	{
		now_ns_ += advance_ms * 1'000'000;
		descriptors_.clear();
		std::uint32_t timeout = 0;
		slirp_pollfds_fill_socket(slirp_, &timeout, add_descriptor, this);
		const int result = poll(descriptors_.data(),
					static_cast<nfds_t>(descriptors_.size()),
					wait_ms);
		slirp_pollfds_poll(slirp_, result < 0, descriptor_events, this);
	}

	[[nodiscard]] std::deque<std::vector<std::uint8_t>> &frames() noexcept
	{
		return frames_;
	}

	/* The guest asks for the gateway, so SLiRP learns the guest's MAC. */
	[[nodiscard]] bool resolve()
	{
		std::vector<std::uint8_t> request(42, 0);
		for (std::size_t index = 0; index < 6; ++index) {
			request[index] = 0xff;
			request[6 + index] = guest_mac[index];
			request[22 + index] = guest_mac[index];
		}
		put16(request, 12, 0x0806);
		put16(request, 14, 1);
		put16(request, 16, 0x0800);
		request[18] = 6;
		request[19] = 4;
		put16(request, 20, 1);
		for (std::size_t index = 0; index < 4; ++index) {
			request[28 + index] = guest_ip[index];
			request[38 + index] = gateway_ip[index];
		}
		input(request);
		for (const auto &frame : frames_)
			if (frame.size() >= 42 && get16(frame, 12) == 0x0806 &&
			    get16(frame, 20) == 2) {
				frames_.clear();
				return true;
			}
		return false;
	}

private:
	static slirp_ssize_t send_packet(const void *buffer, size_t length,
					 void *opaque)
	{
		auto &network = *static_cast<Network *>(opaque);
		const auto *bytes = static_cast<const std::uint8_t *>(buffer);
		network.frames_.emplace_back(bytes, bytes + length);
		return static_cast<slirp_ssize_t>(length);
	}

	static void guest_error(const char *message, void *)
	{
		fprintf(stderr, "slirp guest error: %s\n", message);
	}

	static std::int64_t clock(void *opaque)
	{
		return static_cast<Network *>(opaque)->now_ns_;
	}

	static void *timer_new(SlirpTimerId, void *, void *)
	{
		static int timer;
		return &timer;
	}

	static void timer_free(void *, void *) {}
	static void timer_modify(void *, std::int64_t, void *) {}
	static void notify(void *) {}
	static void poll_socket(slirp_os_socket, void *) {}

	static int add_descriptor(slirp_os_socket descriptor, int events,
				  void *opaque)
	{
		auto &network = *static_cast<Network *>(opaque);
		short wanted = 0;
		if ((events & SLIRP_POLL_IN) != 0)
			wanted |= POLLIN;
		if ((events & SLIRP_POLL_OUT) != 0)
			wanted |= POLLOUT;
		if ((events & SLIRP_POLL_PRI) != 0)
			wanted |= POLLPRI;
		network.descriptors_.push_back({descriptor, wanted, 0});
		return static_cast<int>(network.descriptors_.size() - 1);
	}

	static int descriptor_events(int index, void *opaque)
	{
		const auto &network = *static_cast<Network *>(opaque);
		if (index < 0 ||
		    static_cast<std::size_t>(index) >= network.descriptors_.size())
			return 0;
		const short events = network.descriptors_[index].revents;
		int result = 0;
		if ((events & POLLIN) != 0)
			result |= SLIRP_POLL_IN;
		if ((events & POLLOUT) != 0)
			result |= SLIRP_POLL_OUT;
		if ((events & POLLPRI) != 0)
			result |= SLIRP_POLL_PRI;
		if ((events & POLLERR) != 0)
			result |= SLIRP_POLL_ERR;
		if ((events & POLLHUP) != 0)
			result |= SLIRP_POLL_HUP;
		return result;
	}

	static const SlirpCb &callbacks()
	{
		static const SlirpCb value{
			.send_packet = send_packet,
			.guest_error = guest_error,
			.clock_get_ns = clock,
			.timer_free = timer_free,
			.timer_mod = timer_modify,
			.notify = notify,
			.timer_new_opaque = timer_new,
			.register_poll_socket = poll_socket,
			.unregister_poll_socket = poll_socket,
		};
		return value;
	}

	Slirp *slirp_{};
	/* Start well past zero: SLiRP treats a zero fast-timer stamp as unset. */
	std::int64_t now_ns_{1'000'000'000};
	std::vector<pollfd> descriptors_;
	std::deque<std::vector<std::uint8_t>> frames_;
};

} // namespace slirp_harness
