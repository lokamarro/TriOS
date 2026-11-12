// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The guest's name server, answered through the host's system resolver.
 *
 * SLiRP's own DNS proxy reads the host's resolver configuration and sends each
 * guest query to the first configured server, which on most networks is the
 * router: a packet from the application to a local-network address. This
 * resolver instead asks mDNSResponder (dns_sd), so the application itself
 * never addresses the network for a guest's DNS, and the guest follows the
 * device's own resolver configuration (VPN split DNS, encrypted DNS profiles).
 *
 * One thread owns every dns_sd reference, the DNS-over-TCP listener and its
 * connections. SLiRP's worker hands it UDP queries; replies go to the guest
 * through the frame sink, which must be safe to call from this thread.
 */

#include "dns_frames.hpp"
#include "dns_message.hpp"
#include "platform/descriptor.hpp"
#include "socket_paths.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef UML_SLIRP_TESTING
#define UML_SLIRP_TESTING 0
#endif

typedef struct _DNSServiceRef_t *DNSServiceRef;

namespace uml::network {

/* The DNS-over-TCP socket is named dns-<pid> for its process. The name stays
 * the same across a guest restart, which the process audit compares by name,
 * and a run killed outright leaves its socket for the next run to remove from
 * the directory it binds in. */
inline constexpr std::string_view dns_stream_prefix = "dns-";

/* Queries in flight at once, UDP and TCP together; more are refused. */
inline constexpr std::size_t dns_pending_limit = 256;
/* Concurrent DNS-over-TCP connections from the guest. */
inline constexpr std::size_t dns_stream_limit = 16;
/* A query the system resolver has not answered by then gets SERVFAIL. A
 * stub resolver's own timeout (Android: 5 s, two tries) falls inside it. */
inline constexpr std::chrono::milliseconds dns_query_deadline{10'000};
/* An idle DNS-over-TCP connection is closed (RFC 7766 6.2.3). */
inline constexpr std::chrono::milliseconds dns_stream_idle{10'000};

class HostResolver final {
public:
	using FrameSink = std::function<void(std::span<const std::uint8_t>)>;

	HostResolver();
	HostResolver(const HostResolver &) = delete;
	HostResolver &operator=(const HostResolver &) = delete;
	~HostResolver();

	/*
	 * Starts the resolver thread. `stream_paths` are candidates, in order,
	 * for the Unix socket SLiRP's guest forward connects to for DNS over
	 * TCP; the first that binds is used, and none leaves TCP unanswered.
	 * The caller blocks the guest timer around this call, as for any host
	 * thread.
	 */
	int start(FrameSink sink, const std::vector<std::string> &stream_paths);
	/* Cancels every query, closes every connection and joins the thread;
	 * no frame reaches the sink after it returns. */
	void stop() noexcept;
	[[nodiscard]] bool streams_available() const noexcept
	{
		return listener_.get() >= 0;
	}
	/* The bound DNS-over-TCP socket path, while streams_available(). */
	[[nodiscard]] const std::string &stream_path() const noexcept
	{
		return stream_path_;
	}

	/* A guest UDP query. Called from SLiRP's worker; it copies `message`. */
	void submit(const dns::Endpoints &endpoints,
		    std::span<const std::uint8_t> message);

#if UML_SLIRP_TESTING
	/* kDNSServiceInterfaceIndexLocalOnly keeps a probe off the network. */
	void set_interface_for_testing(std::uint32_t index) noexcept
	{
		interface_index_ = index;
	}
	[[nodiscard]] std::uint64_t resolved_for_testing() const noexcept;
#endif

	/* Public for the dns_sd callback; not part of the interface. */
	struct Pending;
	void record(Pending &pending, std::uint32_t flags, std::int32_t error,
		    const char *fullname, std::uint16_t type,
		    std::uint16_t klass, std::span<const std::uint8_t> data,
		    std::uint32_t ttl) noexcept;

private:
	using Clock = std::chrono::steady_clock;

	struct Incoming final {
		dns::Endpoints endpoints;
		std::vector<std::uint8_t> message;
	};

	struct Stream final {
		std::uint64_t serial{};
		uml::platform::UniqueDescriptor descriptor;
		std::vector<std::uint8_t> input;
		std::vector<std::uint8_t> output;
		Clock::time_point active{};
		std::size_t pending{};
		/* The guest shut down its sending side (a FIN follows its last
		 * query): nothing more arrives, and the answers still owed
		 * go out before the connection closes. */
		bool input_closed{};
	};

	void run(std::stop_token stop_token) noexcept;
	void wake() noexcept;
	void handle(std::optional<dns::Endpoints> datagram, std::uint64_t stream,
		    std::span<const std::uint8_t> message);
	void respond(const std::optional<dns::Endpoints> &datagram,
		     std::uint64_t stream, std::span<const std::uint8_t> reply);
	void finish(Pending &pending) noexcept;
	void complete_ready() noexcept;
	void expire(Clock::time_point now) noexcept;
	void drop_connection() noexcept;
	bool ensure_connection() noexcept;
	void accept_streams() noexcept;
	void read_stream(Stream &stream) noexcept;
	void flush_stream(Stream &stream) noexcept;
	void close_streams() noexcept;
	int bind_listener(const std::string &path) noexcept;
	Stream *find_stream(std::uint64_t serial) noexcept;

	FrameSink sink_;
	std::jthread worker_;
	std::array<uml::platform::UniqueDescriptor, 2> wake_pipe_;
	uml::platform::UniqueDescriptor listener_;
	std::unique_ptr<uml::device::SocketPath> listener_path_;
	std::string stream_path_;
	std::mutex inbox_mutex_;
	std::deque<Incoming> inbox_;
	bool accepting_{};

	/* Owned by the resolver thread alone. */
	DNSServiceRef connection_{};
	std::list<std::unique_ptr<Pending>> pending_;
	std::list<Stream> streams_;
	std::uint64_t next_stream_{1};
	std::uint16_t next_identification_{1};
	bool batch_ended_{};
	std::uint32_t interface_index_{};
#if UML_SLIRP_TESTING
	std::atomic_uint64_t resolved_for_testing_{};
#endif
};

} // namespace uml::network
