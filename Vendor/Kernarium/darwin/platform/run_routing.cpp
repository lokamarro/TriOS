// SPDX-License-Identifier: GPL-2.0-or-later
#include "run_routing.hpp"

#if UML_RUN_ROUTING_TESTING
#include "run_routing_testing.hpp"

#include <chrono>
#include <condition_variable>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace uml::routing {
namespace {

/* Writers hold `mutex`. What an unbound thread resolves is published as a
 * sequence-stamped copy beside it, because every vCPU carrier is such a thread
 * and resolves its route on every software-TLB miss: a process-wide mutex
 * there is contended in proportion to the vCPU count. A reader copies values
 * and never follows `sole_fallback`, so a scope may end while it reads. */
struct RouteRegistry final {
	std::mutex mutex;
	std::size_t active_count{};
	ActiveRouteScope *sole_fallback{};
	std::atomic<std::uint64_t> sequence{};
	std::atomic<void *> published_runtime{};
	std::atomic<void *> published_network{};
};

/* Public routes can outlive ordinary static destruction during process exit.
 * Give the synchronization object explicit process lifetime. */
RouteRegistry *const route_registry = new RouteRegistry;
thread_local ActiveRouteScope *thread_route;

#if UML_RUN_ROUTING_TESTING
struct TestTransactionBarrier final {
	std::mutex mutex;
	std::condition_variable changed;
	testing::TransactionBoundary boundary{
		testing::TransactionBoundary::commit};
	bool armed{};
	bool paused{};
	bool resolver_waiting{};
};

TestTransactionBarrier *const test_barrier = new TestTransactionBarrier;

void pause_transaction(testing::TransactionBoundary boundary) noexcept
{
	std::unique_lock lock{test_barrier->mutex};
	if (!test_barrier->armed || test_barrier->boundary != boundary)
		return;
	test_barrier->paused = true;
	test_barrier->changed.notify_all();
	test_barrier->changed.wait(lock, [] {
		return test_barrier->resolver_waiting || !test_barrier->armed;
	});
	test_barrier->armed = false;
	test_barrier->paused = false;
	test_barrier->resolver_waiting = false;
}

void announce_resolver() noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	if (!test_barrier->paused)
		return;
	test_barrier->resolver_waiting = true;
	test_barrier->changed.notify_all();
}
#endif

/* One registry change, from the first field it touches to the last, including
 * the testing pause: a reader waits the odd sequence out and then sees the
 * change whole. The caller holds the registry mutex. The release fence after
 * the opening increment pairs with the reader's acquire fence: a reader that
 * loads any payload store of this change then sees the odd sequence (or a
 * later one) in its closing check and retries. */
class RoutePublication final {
public:
	RoutePublication() noexcept
	{
		route_registry->sequence.fetch_add(1, std::memory_order_seq_cst);
		std::atomic_thread_fence(std::memory_order_release);
	}

	~RoutePublication()
	{
		route_registry->sequence.fetch_add(1, std::memory_order_seq_cst);
	}

	RoutePublication(const RoutePublication &) = delete;
	RoutePublication &operator=(const RoutePublication &) = delete;

	static void publish(const Route &fallback, bool has_fallback) noexcept
	{
		route_registry->published_runtime.store(
			has_fallback ? fallback.runtime : nullptr,
			std::memory_order_relaxed);
		route_registry->published_network.store(
			has_fallback ? fallback.network : nullptr,
			std::memory_order_relaxed);
	}
};

} // namespace

ActiveRouteScope::ActiveRouteScope(void *runtime, void *network) noexcept
	: ActiveRouteScope{runtime, network, DeferredPublication{}}
{
	commit();
}

ActiveRouteScope::ActiveRouteScope(void *runtime, void *network,
	DeferredPublication) noexcept
	: route_{runtime, network}
{
}

void ActiveRouteScope::commit() noexcept
{
	if (committed_)
		return;
	std::lock_guard lock{route_registry->mutex};
	RoutePublication publication;
	previous_thread_ = thread_route;
	route_registry->sole_fallback =
		route_registry->active_count == 0 ? this : nullptr;
	++route_registry->active_count;
	RoutePublication::publish(
		route_, route_registry->sole_fallback != nullptr);
	committed_ = true;
	thread_route = this;
#if UML_RUN_ROUTING_TESTING
	pause_transaction(testing::TransactionBoundary::commit);
#endif
}

ActiveRouteScope::~ActiveRouteScope()
{
	if (committed_) {
		std::lock_guard lock{route_registry->mutex};
		RoutePublication publication;
		if (route_registry->sole_fallback == this)
			route_registry->sole_fallback = nullptr;
		--route_registry->active_count;
		RoutePublication::publish({},
			route_registry->sole_fallback != nullptr);
#if UML_RUN_ROUTING_TESTING
		pause_transaction(testing::TransactionBoundary::retirement);
#endif
	}
	if (committed_)
		thread_route = previous_thread_;
}

Route current_route() noexcept
{
	if (thread_route != nullptr)
		return thread_route->route_;
#if UML_RUN_ROUTING_TESTING
	announce_resolver();
#endif
	for (unsigned attempt = 0;; ++attempt) {
		const auto before = route_registry->sequence.load(
			std::memory_order_acquire);
		if ((before & 1U) == 0) {
			const Route route{
				route_registry->published_runtime.load(
					std::memory_order_relaxed),
				route_registry->published_network.load(
					std::memory_order_relaxed),
			};
			/* An acquire load orders only what follows it, so the
			 * payload loads above need this fence to stay ahead of
			 * the closing check. */
			std::atomic_thread_fence(std::memory_order_acquire);
			if (route_registry->sequence.load(
				    std::memory_order_relaxed) == before)
				return route;
		}
		if (attempt >= 64)
			std::this_thread::yield();
	}
}

#if UML_RUN_ROUTING_TESTING
namespace testing {

void pause_next_transaction(TransactionBoundary boundary) noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	test_barrier->boundary = boundary;
	test_barrier->armed = true;
	test_barrier->paused = false;
	test_barrier->resolver_waiting = false;
}

bool wait_for_transaction_pause(unsigned int timeout_milliseconds) noexcept
{
	std::unique_lock lock{test_barrier->mutex};
	return test_barrier->changed.wait_for(lock,
		std::chrono::milliseconds{timeout_milliseconds}, [] {
			return test_barrier->paused;
		});
}

void cancel_transaction_pause() noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	test_barrier->armed = false;
	test_barrier->changed.notify_all();
}

} // namespace testing
#endif

} // namespace uml::routing
