/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "uml_darwin_network.h"
#include "provider.hpp"
#include "uml_slirp_net.h"

#include <arpa/inet.h>
#include <atomic>
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <time.h>


struct network_context {
	void *value = uml_slirp_net_create();

	~network_context()
	{
		if (value != nullptr) {
			uml_slirp_net_stop(value);
			uml_slirp_net_destroy(value);
		}
	}
};

struct arp_frame {
	uint8_t destination[6];
	uint8_t source[6];
	uint16_t ethernet_type;
	uint16_t hardware_type;
	uint16_t protocol_type;
	uint8_t hardware_length;
	uint8_t protocol_length;
	uint16_t operation;
	uint8_t sender_hardware[6];
	uint8_t sender_protocol[4];
	uint8_t target_hardware[6];
	uint8_t target_protocol[4];
} __attribute__((packed));

static void make_request(struct arp_frame *request, const uint8_t mac[6])
{
	static const uint8_t broadcast[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	static const uint8_t guest_address[4] = { 10, 0, 2, 15 };
	static const uint8_t gateway_address[4] = { 10, 0, 2, 2 };

	memcpy(request->destination, broadcast, sizeof(broadcast));
	memcpy(request->source, mac, sizeof(request->source));
	request->ethernet_type = htons(0x0806);
	request->hardware_type = htons(1);
	request->protocol_type = htons(0x0800);
	request->hardware_length = 6;
	request->protocol_length = 4;
	request->operation = htons(1);
	memcpy(request->sender_hardware, mac,
	       sizeof(request->sender_hardware));
	memcpy(request->sender_protocol, guest_address,
	       sizeof(request->sender_protocol));
	memcpy(request->target_protocol, gateway_address,
	       sizeof(request->target_protocol));
}

static int wait_for_irq(void *network, int timeout_ms)
{
	struct pollfd descriptor = {
		.fd = uml_slirp_net_irq_fd(network),
		.events = POLLIN,
	};

	return descriptor.fd >= 0 && poll(&descriptor, 1, timeout_ms) == 1 &&
	       (descriptor.revents & POLLIN) != 0;
}

static size_t descriptor_count()
{
	size_t count = 0;
	for (const auto &entry : std::filesystem::directory_iterator{"/dev/fd"}) {
		(void)entry;
		++count;
	}
	return count;
}

static bool write_cannot_commit_after_stop()
{
	network_context network;
	uint8_t mac[6]{};
	unsigned int mtu = 0;
	if (network.value == nullptr ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0)
		return false;
	auto *provider =
		static_cast<uml::network::SlirpProvider *>(network.value);
	const auto processed_before =
		provider->processed_transmits_for_testing();
	const int irq = uml_slirp_net_irq_fd(network.value);
	const int irq_flags = irq < 0 ? -1 : fcntl(irq, F_GETFD);
	if (irq_flags < 0 || (irq_flags & FD_CLOEXEC) == 0)
		return false;
	std::array<std::byte, 64> frame{};
	std::atomic_int result{-EINPROGRESS};
	provider->pause_next_write_before_commit_for_testing();
	std::thread writer{[&] {
		result.store(provider->write(frame), std::memory_order_release);
	}};
	if (!provider->wait_for_write_pause_for_testing()) {
		provider->release_write_for_testing();
		writer.join();
		return false;
	}
	uml_slirp_net_stop(network.value);
	provider->release_write_for_testing();
	writer.join();
	if (result.load(std::memory_order_acquire) != -EINVAL ||
	    !provider->readiness_channels_drained_for_testing() ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0)
		return false;
	struct timespec delay = {.tv_nsec = 1'000'000};
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (provider->processed_transmits_for_testing() != processed_before)
			return false;
		(void)nanosleep(&delay, nullptr);
	}
	std::array<std::byte, 64> received{};
	return !wait_for_irq(network.value, 0) &&
		provider->read(received) == 0;
}

static bool read_owns_notification_until_completion()
{
	network_context network;
	uint8_t mac[6]{};
	unsigned int mtu = 0;
	if (network.value == nullptr ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0)
		return false;
	auto *provider =
		static_cast<uml::network::SlirpProvider *>(network.value);
	const int stable_irq = uml_slirp_net_irq_fd(network.value);
	std::array<std::byte, 64> frame{};
	frame[0] = std::byte{0x5a};
	if (stable_irq < 0 ||
	    provider->enqueue_received_for_testing(frame) !=
		static_cast<slirp_ssize_t>(frame.size()) ||
	    !wait_for_irq(network.value, 1000))
		return false;

	std::array<std::byte, 64> received{};
	std::atomic_int read_result{-EINPROGRESS};
	provider->pause_next_read_before_notification_for_testing();
	std::thread reader{[&] {
		read_result.store(
			provider->read(received), std::memory_order_release);
	}};
	if (!provider->wait_for_read_pause_for_testing()) {
		provider->release_read_for_testing();
		reader.join();
		return false;
	}
	std::atomic_bool stop_finished{};
	std::thread stopper{[&] {
		uml_slirp_net_stop(network.value);
		stop_finished.store(true, std::memory_order_release);
	}};
	if (!provider->wait_for_receive_cleanup_for_testing() ||
	    stop_finished.load(std::memory_order_acquire)) {
		provider->release_read_for_testing();
		reader.join();
		stopper.join();
		return false;
	}
	provider->release_read_for_testing();
	reader.join();
	stopper.join();
	if (read_result.load(std::memory_order_acquire) !=
		static_cast<int>(frame.size()) ||
	    received.front() != frame.front() ||
	    !stop_finished.load(std::memory_order_acquire) ||
	    !provider->readiness_channels_drained_for_testing() ||
	    fcntl(stable_irq, F_GETFD) < 0 ||
	    uml_slirp_net_irq_fd(network.value) != -1 ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0 ||
	    uml_slirp_net_irq_fd(network.value) != stable_irq)
		return false;
	return !wait_for_irq(network.value, 0) &&
		provider->read(received) == 0;
}

static bool stop_reopen_interleaves_are_descriptor_balanced()
{
	/* Let libslirp perform any process-lifetime initialization before the
	 * provider-owned descriptor balance is sampled. */
	{
		network_context warmup;
		uint8_t mac[6]{};
		unsigned int mtu = 0;
		if (warmup.value == nullptr ||
		    uml_slirp_net_open(warmup.value, mac, &mtu) != 0)
			return false;
	}
	const auto before = descriptor_count();
	const bool passed = write_cannot_commit_after_stop() &&
		read_owns_notification_until_completion();
	return passed && descriptor_count() == before;
}

int main(void)
{
	static const uint8_t gateway_address[4] = { 10, 0, 2, 2 };
	struct timespec delay = { .tv_nsec = 10000000 };
	uint8_t packet[2048] = { };
	struct arp_frame *request = (struct arp_frame *)packet;
	uint8_t mac[6];
	unsigned int mtu;
	int attempt;
	int length;
	network_context network;

	if (!stop_reopen_interleaves_are_descriptor_balanced()) {
		fprintf(stderr,
			"network stop/reopen I/O ownership interleave failed\n");
		return 1;
	}

	if (network.value == nullptr ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0) {
		fprintf(stderr, "network open failed\n");
		return 1;
	}
	make_request(request, mac);
	if (uml_slirp_net_write(network.value, packet, 64) != 0) {
		fprintf(stderr, "first-run network transmit failed\n");
		return 1;
	}
	if (!wait_for_irq(network.value, 2000)) {
		fprintf(stderr, "first-run network IRQ timed out\n");
		return 1;
	}
	/* Deliberately leave the first response and IRQ unread.  A restarted
	 * provider must not expose either object to the next emulator run. */
	uml_slirp_net_stop(network.value);
	if (uml_slirp_net_open(network.value, mac, &mtu) != 0) {
		fprintf(stderr, "second-run network open failed\n");
		return 1;
	}
	if (wait_for_irq(network.value, 0) ||
	    uml_slirp_net_read(network.value, packet, sizeof(packet)) != 0) {
		fprintf(stderr, "second run inherited a stale frame or IRQ\n");
		return 1;
	}
	auto *provider = static_cast<uml::network::SlirpProvider *>(network.value);
	const uint64_t completed_before_pause =
		provider->processed_transmits_for_testing();
	provider->pause_transmit_for_testing();
	memset(packet, 0, sizeof(packet));
	request = (struct arp_frame *)packet;
	make_request(request, mac);
	if (uml_slirp_net_write(network.value, packet, 64) != 0 ||
	    !provider->wait_for_transmit_pause_for_testing()) {
		fprintf(stderr, "queued transmit did not reach deterministic pause\n");
		return 1;
	}
	uml_slirp_net_stop(network.value);
	if (provider->processed_transmits_for_testing() !=
	    completed_before_pause ||
	    uml_slirp_net_open(network.value, mac, &mtu) != 0 ||
	    wait_for_irq(network.value, 0) ||
	    uml_slirp_net_read(network.value, packet, sizeof(packet)) != 0) {
		fprintf(stderr,
			"reopen executed or exposed a prior-session transmit\n");
		return 1;
	}
	memset(packet, 0, sizeof(packet));
	request = (struct arp_frame *)packet;
	make_request(request, mac);
	if (uml_slirp_net_write(network.value, packet, 64) != 0) {
		fprintf(stderr, "second-run network transmit failed\n");
		return 1;
	}
	if (!wait_for_irq(network.value, 2000)) {
		fprintf(stderr, "second-run network IRQ timed out\n");
		return 1;
	}
	{
		uint8_t short_buffer[1];
		if (uml_slirp_net_read(
			    network.value, short_buffer, sizeof(short_buffer)) !=
			    -EMSGSIZE ||
		    !wait_for_irq(network.value, 0)) {
			fprintf(stderr,
				"short network read consumed the frame or IRQ\n");
			return 1;
		}
	}
	for (attempt = 0; attempt < 200; attempt++) {
		struct arp_frame *reply;

		(void)nanosleep(&delay, NULL);
		length = uml_slirp_net_read(
			network.value, packet, sizeof(packet));
		if (length < (int)sizeof(*reply))
			continue;
		reply = (struct arp_frame *)packet;
		if (reply->ethernet_type != htons(0x0806) ||
		    reply->operation != htons(2) ||
		    memcmp(reply->sender_protocol, gateway_address,
			   sizeof(gateway_address)) != 0)
			continue;
		printf("PASS: user-mode NAT restart preserved descriptor ownership, "
		       "discarded stale state and a paused prior-session transmit, "
		       "then answered gateway ARP; "
		       "guest mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
		       mtu, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		return 0;
	}
	fprintf(stderr, "no gateway ARP response\n");
	return 1;
}
