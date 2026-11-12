/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest transmit into a full ring waits for the SLiRP event loop instead of
 * being dropped: the guest driver has no queue flow control, so a refused
 * frame is a lost packet. The event loop is parked at its next dequeue, which
 * makes a full ring deterministic.
 */
#include "provider.hpp"
#include "uml_slirp_net.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <errno.h>
#include <thread>

namespace {

using uml::network::SlirpProvider;
using Clock = std::chrono::steady_clock;

struct NetworkContext final {
	void *value = uml_slirp_net_create();

	~NetworkContext()
	{
		if (value != nullptr) {
			uml_slirp_net_stop(value);
			uml_slirp_net_destroy(value);
		}
	}
};

/* An all-zero Ethernet frame has no protocol SLiRP handles, so the event loop
 * consumes and discards it. */
const std::array<std::uint8_t, 64> frame{};

/* Every frame the ring has accepted, from any writer. */
std::atomic<std::uint64_t> accepted_frames{0};

int write_frame(void *network)
{
	const int result =
		uml_slirp_net_write(network, frame.data(), frame.size());
	if (result == 0)
		accepted_frames.fetch_add(1, std::memory_order_relaxed);
	return result;
}

/* Poll a condition the event loop makes true, for at most two seconds. */
template <class Ready> bool wait_until(Ready ready)
{
	const auto deadline = Clock::now() + std::chrono::seconds{2};
	while (!ready()) {
		if (Clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	}
	return true;
}

/* A resumed loop still has the frames of the last fill to take. A pause
 * requested before it has taken them all parks it with some still queued,
 * and the ring can then no longer hold its capacity. */
bool wait_for_drained_ring(SlirpProvider &provider)
{
	return wait_until([&provider] {
		return provider.processed_transmits_for_testing() ==
		       accepted_frames.load(std::memory_order_relaxed);
	});
}

/* Park the event loop and leave the ring holding its capacity. */
bool fill_parked_ring(void *network, SlirpProvider &provider)
{
	const auto processed = provider.processed_transmits_for_testing();
	provider.pause_transmit_for_testing();
	if (write_frame(network) != 0 ||
	    !provider.wait_for_transmit_pause_for_testing())
		return false;
	/* The loop normally parks before dequeuing that frame, but may take it
	 * first; the ring keeps one slot empty to tell full from empty. */
	const std::size_t queued =
		provider.processed_transmits_for_testing() == processed ? 1 : 0;
	for (std::size_t index = queued + 1; index < uml::network::ring_size;
	     ++index)
		if (write_frame(network) != 0)
			return false;
	return true;
}

bool wait_for_one_waiter(SlirpProvider &provider)
{
	return wait_until([&provider] {
		return provider.transmit_space_waiters_for_testing() == 1;
	});
}

bool fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	return false;
}

bool run(void *network, SlirpProvider &provider)
{
	unsigned char mac[6];
	unsigned int mtu;
	if (uml_slirp_net_open(network, mac, &mtu) != 0)
		return fail("network open failed");
	if (!fill_parked_ring(network, provider))
		return fail("a parked ring did not accept its capacity");

	const auto started = Clock::now();
	if (write_frame(network) != -EAGAIN)
		return fail("a full ring with a stalled loop accepted a frame");
	const auto waited = Clock::now() - started;
	if (waited < uml::network::transmit_space_wait / 2 ||
	    waited > std::chrono::seconds{2})
		return fail("a full ring refused a frame without its bounded wait");

	std::atomic_int result{1};
	std::thread writer{[&] { result.store(write_frame(network)); }};
	const bool waiting = wait_for_one_waiter(provider);
	provider.resume_transmit_for_testing();
	writer.join();
	if (!waiting)
		return fail("a writer did not wait on the full ring");
	if (result.load() != 0)
		return fail("a waiting writer was refused after the loop drained");

	if (!wait_for_drained_ring(provider))
		return fail("the resumed loop did not drain the ring");
	if (!fill_parked_ring(network, provider))
		return fail("the ring could not be filled a second time");
	writer = std::thread{[&] { result.store(write_frame(network)); }};
	const bool waiting_at_stop = wait_for_one_waiter(provider);
	uml_slirp_net_stop(network);
	writer.join();
	if (!waiting_at_stop)
		return fail("a writer did not wait before stop");
	if (result.load() != -EINVAL)
		return fail("stop did not release a waiting writer");
	return true;
}

} // namespace

int main()
{
	NetworkContext network;
	if (network.value == nullptr)
		return 1;
	if (!run(network.value, *static_cast<SlirpProvider *>(network.value)))
		return 1;
	std::puts("PASS: a full transmit ring waits for the event loop, "
		  "is bounded, and is released by stop");
	return 0;
}
