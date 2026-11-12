// SPDX-License-Identifier: GPL-2.0-or-later
#include "provider.hpp"
#include "uml_slirp_net.h"
#include "platform/guest_timer_mask.hpp"
#include "socket_paths.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <pthread.h>
#include <signal.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uml::network {
namespace {

using uml::platform::UniqueDescriptor;

constexpr std::array<unsigned char, 6> default_mac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

bool create_pipe(std::array<UniqueDescriptor, 2> &result) noexcept
{
	std::array<int, 2> descriptors{};
	if (pipe(descriptors.data()) != 0)
		return false;
	result[0].reset(descriptors[0]);
	result[1].reset(descriptors[1]);
	using uml::platform::configureDescriptor;
	if (configureDescriptor(result[0].get(), {.closeOnExec = true}) &&
	    configureDescriptor(result[1].get(), {.closeOnExec = true}) &&
	    configureDescriptor(result[0].get(), {.nonblocking = true}) &&
	    configureDescriptor(result[1].get(), {.nonblocking = true}))
		return true;
	const int error = errno;
	result[0].reset();
	result[1].reset();
	errno = error;
	return false;
}

bool ensure_pipe(std::array<UniqueDescriptor, 2> &result) noexcept
{
	if (result[0].get() >= 0 && result[1].get() >= 0)
		return true;
	result[0].reset();
	result[1].reset();
	return create_pipe(result);
}

/* Where SLiRP's guest forward may reach the resolver for DNS over TCP, in
 * order: the application's temporary directory as $TMPDIR names it, Darwin's
 * per-user one, then /tmp. A Unix socket path must fit sun_path (103 bytes
 * and a terminator). An iOS device container's tmp/ is 89 bytes, which the
 * short name fits; an iOS Simulator container's is about 164, so there the
 * resolver binds in /tmp, which the Simulator's processes share with the Mac. */
std::vector<std::string> dns_stream_paths()
{
	const auto name = std::string{dns_stream_prefix} +
		std::to_string(::getpid());
	std::vector<std::string> paths;
	const auto add = [&](std::string directory) {
		if (directory.empty() || directory.front() != '/')
			return;
		if (directory.back() != '/')
			directory.push_back('/');
		auto path = directory + name;
		if (path.size() < sizeof(sockaddr_un::sun_path) &&
		    std::ranges::find(paths, path) == paths.end())
			paths.push_back(std::move(path));
	};
	if (const char *temporary = std::getenv("TMPDIR"); temporary != nullptr)
		add(temporary);
	add(uml::device::user_temporary_directory());
	add("/tmp");
	return paths;
}

void drain_pipe(UniqueDescriptor &descriptor) noexcept
{
	std::array<std::byte, 64> buffer{};
	while (descriptor.get() >= 0 &&
	       ::read(descriptor.get(), buffer.data(), buffer.size()) > 0)
		;
}

} // namespace

void SlirpDeleter::operator()(Slirp *slirp) const noexcept
{
	if (slirp != nullptr)
		slirp_cleanup(slirp);
}

SlirpProvider::~SlirpProvider()
{
	stop();
}

int SlirpProvider::open(std::span<unsigned char, 6> mac,
                       unsigned int &mtu)
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire)) {
		stopping_.store(false, std::memory_order_release);
		/* Both pipes are provider-owned capabilities. Keep their numeric
		 * descriptors stable across stop/reopen so an IRQ registration that
		 * borrowed receive_pipe_[0] can never observe descriptor reuse between
		 * sessions. Failed first-open attempts may retain the same owned pipes
		 * for a later retry. */
		if (!ensure_pipe(wake_pipe_) || !ensure_pipe(receive_pipe_))
			return errno == 0 ? -EIO : -errno;

		SlirpConfig config{};
		config.version = 6;
		config.in_enabled = true;
		config.restricted = 0;
		/* SLiRP's DNS proxy sends a guest's queries to the host's first
		 * configured server, usually a local-network router. The
		 * provider answers the name server itself (answer_locally and
		 * HostResolver), so SLiRP never addresses that server. */
		config.disable_dns = true;
		(void)inet_pton(AF_INET, network_address, &config.vnetwork);
		(void)inet_pton(AF_INET, network_mask, &config.vnetmask);
		(void)inet_pton(AF_INET, host_alias_address, &config.vhost);
		(void)inet_pton(AF_INET, guest_address, &config.vdhcp_start);
		(void)inet_pton(AF_INET, nameserver_address, &config.vnameserver);
		config.vhostname = "uml-darwin";
		config.if_mtu = 1500;
		config.if_mru = 1500;
		slirp_.reset(slirp_new(&config, &callbacks(), this));
		if (!slirp_) {
			return -EIO;
		}
		/* Replay the recorded forwards onto the SLiRP instance this open
		 * created, before its worker exists. libslirp takes no lock: a
		 * forward links a listening socket into the lists the worker walks,
		 * and frees it again when the host port is taken. A refused forward
		 * is reported but never fails the guest network: losing one
		 * convenience port must not cost every guest its only interface. */
		for (const auto &forward : forwards_) {
			if (install_forward(forward) == 0)
				continue;
			std::fprintf(stderr,
				"Unable to forward host %s:%u to guest %s:%u\n",
				forward.host_address.c_str(),
				static_cast<unsigned int>(forward.host_port),
				forward.guest_address.c_str(),
				static_cast<unsigned int>(forward.guest_port));
		}
		try {
			/*
			 * ITIMER_REAL is UML's guest clock and must run only on its
			 * designated CPU pthread. Darwin otherwise may select this
			 * SLiRP worker, or the resolver's thread, for SIGALRM and
			 * enter Linux scheduler code on a foreign host stack.
			 */
			const uml::platform::ScopedGuestTimerBlock timer_block(
				"blocking the UML timer around the SLiRP worker");
#if UML_SLIRP_TESTING
			resolver_.set_interface_for_testing(
				dns_interface_for_testing_);
			const auto stream_paths = dns_streams_unavailable_for_testing_ ?
				std::vector<std::string>{} : dns_stream_paths();
#else
			const auto stream_paths = dns_stream_paths();
#endif
			if (resolver_.start(
				    [this](std::span<const std::uint8_t> frame) {
					    try {
						    (void)enqueue_received(
							    std::as_bytes(frame));
					    } catch (...) {
					    }
				    },
				    stream_paths) != 0) {
				slirp_.reset();
				return -EIO;
			}
			/* DNS over TCP to the name server, and to the gateway as
			 * SLiRP's own proxy allowed, reaches the resolver through a
			 * per-connection Unix socket. disable_dns is what lets a
			 * guest forward claim the name server's port. */
			name_server_stream_ = false;
			gateway_stream_ = false;
			if (resolver_.streams_available()) {
				const std::array<std::pair<const char *, bool *>, 2>
					servers{{{nameserver_address,
						  &name_server_stream_},
						 {host_alias_address,
						  &gateway_stream_}}};
				for (const auto &[server, installed] : servers) {
					in_addr address{};
					(void)inet_pton(AF_INET, server, &address);
					*installed = slirp_add_unix(
						slirp_.get(),
						resolver_.stream_path().c_str(),
						&address, 53) == 0;
					if (!*installed)
						std::fprintf(stderr,
							"Darwin user network: no DNS "
							"over TCP at %s\n", server);
				}
			}
			worker_ = std::jthread{[this](std::stop_token stop_token) {
				event_loop(stop_token);
			}};
		} catch (const std::system_error &) {
			resolver_.stop();
			slirp_.reset();
			return -EIO;
		}
		started_.store(true, std::memory_order_release);
	}
	std::ranges::copy(default_mac, mac.begin());
	mtu = 1500;
	return 0;
}

int SlirpProvider::install_forward(const HostForward &forward) noexcept
{
	in_addr host_address{};
	in_addr target_address{};

	if (inet_pton(AF_INET, forward.host_address.c_str(), &host_address) != 1)
		return -EINVAL;
	if (inet_pton(AF_INET, forward.guest_address.c_str(), &target_address) != 1)
		return -EINVAL;
#if UML_SLIRP_TESTING
	if (worker_.joinable())
		forwards_installed_while_running_for_testing_.fetch_add(
			1, std::memory_order_relaxed);
#endif
	if (slirp_add_hostfwd(slirp_.get(), forward.udp ? 1 : 0, host_address,
			      forward.host_port, target_address,
			      forward.guest_port) != 0)
		return errno == 0 ? -EADDRINUSE : -errno;
	return 0;
}

int SlirpProvider::add_forward(const HostForward &forward)
{
	std::lock_guard lock{start_mutex_};
	if (forward.host_port == 0 || forward.guest_port == 0)
		return -EINVAL;
	/* A running worker walks the instance's socket lists without a lock, so
	 * a forward joins only the next instance open() creates. */
	if (started_.load(std::memory_order_acquire))
		return -EBUSY;
	forwards_.push_back(forward);
	return 0;
}

int SlirpProvider::irq_fd() const
{
	std::lock_guard lock{start_mutex_};
	return started_.load(std::memory_order_acquire) ?
		receive_pipe_[0].get() : -1;
}

void SlirpProvider::stop() noexcept
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire))
		return;
	/* Close new guest I/O admission before waking the worker. Operations that
	 * already committed under a queue mutex retain descriptor ownership until
	 * that mutex is released; cleanup takes the same mutexes below. */
	started_.store(false, std::memory_order_release);
	stopping_.store(true, std::memory_order_release);
	worker_.request_stop();
#if UML_SLIRP_TESTING
	{
		std::lock_guard pause_lock{transmit_pause_mutex_};
		transmit_pause_requested_for_testing_.store(
			false, std::memory_order_release);
	}
	transmit_pause_condition_.notify_all();
#endif
	wake();
	/* Release writers waiting for transmit space. Taking the mutex orders
	 * the started_ store before any waiter's next predicate check. */
	{
		std::lock_guard transmit_lock{transmit_mutex_};
	}
	transmit_space_.notify_all();
	if (worker_.joinable())
		worker_.join();
	/* The resolver delivers into the receive ring from its own thread;
	 * once it is joined nothing can refill the ring cleared below. */
	resolver_.stop();
#if UML_SLIRP_TESTING
	notify_receive_cleanup_for_testing();
#endif
	{
		std::lock_guard receive_lock{receive_mutex_};
		receive_head_ = 0;
		receive_tail_ = 0;
		drain_pipe(receive_pipe_[0]);
	}
	{
		std::lock_guard transmit_lock{transmit_mutex_};
		transmit_head_ = 0;
		transmit_tail_ = 0;
		drain_pipe(wake_pipe_[0]);
	}
	slirp_.reset();
	timers_.clear();
}

#if UML_SLIRP_TESTING
void SlirpProvider::pause_transmit_for_testing() noexcept
{
	{
		std::lock_guard lock{transmit_pause_mutex_};
		transmit_pause_reached_for_testing_ = false;
		transmit_pause_requested_for_testing_.store(
			true, std::memory_order_release);
	}
	wake();
}

bool SlirpProvider::wait_for_transmit_pause_for_testing() noexcept
{
	std::unique_lock lock{transmit_pause_mutex_};
	return transmit_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return transmit_pause_reached_for_testing_;
		});
}

void SlirpProvider::resume_transmit_for_testing() noexcept
{
	{
		std::lock_guard lock{transmit_pause_mutex_};
		transmit_pause_requested_for_testing_.store(
			false, std::memory_order_release);
	}
	transmit_pause_condition_.notify_all();
}

std::size_t SlirpProvider::transmit_space_waiters_for_testing() noexcept
{
	std::lock_guard lock{transmit_mutex_};
	return transmit_space_waiters_;
}

void SlirpProvider::pause_next_write_before_commit_for_testing() noexcept
{
	std::lock_guard lock{io_pause_mutex_};
	write_pause_requested_for_testing_ = true;
	write_pause_reached_for_testing_ = false;
	release_write_for_testing_ = false;
}

bool SlirpProvider::wait_for_write_pause_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return write_pause_reached_for_testing_;
		});
}

void SlirpProvider::release_write_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		release_write_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}

void SlirpProvider::pause_next_read_before_notification_for_testing() noexcept
{
	std::lock_guard lock{io_pause_mutex_};
	read_pause_requested_for_testing_ = true;
	read_pause_reached_for_testing_ = false;
	release_read_for_testing_ = false;
	receive_cleanup_reached_for_testing_ = false;
}

bool SlirpProvider::wait_for_read_pause_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return read_pause_reached_for_testing_;
		});
}

void SlirpProvider::release_read_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		release_read_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}

bool SlirpProvider::wait_for_receive_cleanup_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return receive_cleanup_reached_for_testing_;
		});
}

slirp_ssize_t SlirpProvider::enqueue_received_for_testing(
	std::span<const std::byte> frame) noexcept
{
	try {
		return enqueue_received(frame);
	} catch (...) {
		return -1;
	}
}

bool SlirpProvider::readiness_channels_drained_for_testing() const noexcept
{
	std::array<pollfd, 2> descriptors{{
		{.fd = wake_pipe_[0].get(), .events = POLLIN, .revents = 0},
		{.fd = receive_pipe_[0].get(), .events = POLLIN, .revents = 0},
	}};
	return descriptors[0].fd >= 0 && descriptors[1].fd >= 0 &&
		::poll(descriptors.data(), descriptors.size(), 0) == 0;
}

void SlirpProvider::pause_write_before_commit_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	if (!write_pause_requested_for_testing_)
		return;
	write_pause_requested_for_testing_ = false;
	write_pause_reached_for_testing_ = true;
	io_pause_condition_.notify_all();
	io_pause_condition_.wait(lock, [this] {
		return release_write_for_testing_;
	});
}

void SlirpProvider::pause_read_before_notification_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	if (!read_pause_requested_for_testing_)
		return;
	read_pause_requested_for_testing_ = false;
	read_pause_reached_for_testing_ = true;
	io_pause_condition_.notify_all();
	io_pause_condition_.wait(lock, [this] {
		return release_read_for_testing_;
	});
}

void SlirpProvider::notify_receive_cleanup_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		receive_cleanup_reached_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}
#endif

} // namespace uml::network

namespace uml::network {

bool parse_host_forward(
	std::string_view specification, HostForward &forward) noexcept
{
	const auto port_of = [](std::string_view text,
				std::uint16_t &port) noexcept {
		unsigned int value = 0;
		if (text.empty())
			return false;
		const auto *first = text.data();
		const auto *last = first + text.size();
		const auto result = std::from_chars(first, last, value);
		if (result.ec != std::errc{} || result.ptr != last)
			return false;
		if (value == 0 || value > 65535U)
			return false;
		port = static_cast<std::uint16_t>(value);
		return true;
	};
	const auto address_and_port = [&](std::string_view side,
					  std::string &address,
					  std::uint16_t &port) noexcept {
		const auto separator = side.rfind(':');
		if (separator == std::string_view::npos)
			return port_of(side, port);
		const auto text = side.substr(0, separator);
		if (text.empty())
			return false;
		in_addr parsed{};
		const std::string candidate{text};
		if (inet_pton(AF_INET, candidate.c_str(), &parsed) != 1)
			return false;
		address = candidate;
		return port_of(side.substr(separator + 1), port);
	};

	HostForward parsed{};
	auto remainder = specification;
	if (remainder.starts_with("tcp:")) {
		remainder.remove_prefix(4);
	} else if (remainder.starts_with("udp:")) {
		parsed.udp = true;
		remainder.remove_prefix(4);
	}
	const auto split = remainder.find('-');
	if (split == std::string_view::npos)
		return false;
	if (!address_and_port(remainder.substr(0, split), parsed.host_address,
			      parsed.host_port))
		return false;
	if (!address_and_port(remainder.substr(split + 1), parsed.guest_address,
			      parsed.guest_port))
		return false;
	forward = std::move(parsed);
	return true;
}

} // namespace uml::network

extern "C" int uml_slirp_net_open(
	void *context, unsigned char mac[6], unsigned int *mtu)
{
	if (context == nullptr || mac == nullptr || mtu == nullptr)
		return -EINVAL;
#ifndef UML_APP_STORE_RUNTIME
	if (const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
	    disabled != nullptr && disabled[0] != '\0' && disabled[0] != '0')
		return -ENETDOWN;
#endif
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->open(
			std::span<unsigned char, 6>{mac, 6}, *mtu);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int uml_slirp_net_irq_fd(void *context)
{
	if (context == nullptr)
		return -1;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->irq_fd();
	} catch (...) {
		return -1;
	}
}

extern "C" int uml_slirp_net_read(
	void *context, void *buffer, std::size_t capacity)
{
	if (context == nullptr || (buffer == nullptr && capacity != 0))
		return -EINVAL;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->read(
			{static_cast<std::byte *>(buffer), capacity});
	} catch (...) {
		return -EIO;
	}
}

extern "C" int uml_slirp_net_write(
	void *context, const void *buffer, std::size_t length)
{
	if (context == nullptr || (buffer == nullptr && length != 0))
		return -EINVAL;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->write(
			{static_cast<const std::byte *>(buffer), length});
	} catch (...) {
		return -EIO;
	}
}

extern "C" int uml_slirp_net_add_forward(
	void *context, const char *specification)
{
	if (context == nullptr || specification == nullptr)
		return -EINVAL;
	try {
		uml::network::HostForward forward{};
		if (!uml::network::parse_host_forward(specification, forward))
			return -EINVAL;
		return static_cast<uml::network::SlirpProvider *>(context)
			->add_forward(forward);
	} catch (...) {
		return -EIO;
	}
}

extern "C" void uml_slirp_net_stop(void *context)
{
	if (context != nullptr)
		static_cast<uml::network::SlirpProvider *>(context)->stop();
}

extern "C" void *uml_slirp_net_create(void)
{
	return new (std::nothrow) uml::network::SlirpProvider;
}

extern "C" void uml_slirp_net_destroy(void *context)
{
	delete static_cast<uml::network::SlirpProvider *>(context);
}
