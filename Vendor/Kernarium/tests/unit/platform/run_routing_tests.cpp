// SPDX-License-Identifier: GPL-2.0-or-later
#include "allocation_failure.hpp"
#include "require.hpp"
#include "run_routing.hpp"
#include "run_routing_testing.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using uml::testing::require;

template<typename Action>
uml::routing::Route resolve_during_transaction(
	uml::routing::testing::TransactionBoundary boundary,
	Action &&action, const char *timeout_message)
{
	uml::routing::Route resolution;
	bool paused{};
	uml::routing::testing::pause_next_transaction(boundary);
	std::thread resolver{[&] {
		paused = uml::routing::testing::wait_for_transaction_pause(5000);
		if (paused)
			resolution = uml::routing::current_route();
		else
			uml::routing::testing::cancel_transaction_pause();
	}};
	action();
	resolver.join();
	require(paused, timeout_message);
	return resolution;
}

void require_route(const uml::routing::Route &route, void *runtime,
	void *network, const char *message)
{
	require(route.runtime == runtime && route.network == network, message);
}

void test_publication_and_retirement_are_one_observable_transaction()
{
	int runtime_a;
	int network_a;
	int runtime_b;
	int network_b;
	auto route_a = std::make_unique<uml::routing::ActiveRouteScope>(
		&runtime_a, &network_a);

	uml::routing::Route initial;
	std::thread initial_resolver{[&] {
		initial = uml::routing::current_route();
	}};
	initial_resolver.join();
	require_route(initial, &runtime_a, &network_a,
		"sole route did not publish both endpoints");

	std::mutex release_mutex;
	std::condition_variable release_changed;
	bool release_b{};
	bool b_committed{};
	bool b_bound{};
	std::thread route_b_thread;
	const auto during_commit = resolve_during_transaction(
		uml::routing::testing::TransactionBoundary::commit, [&] {
			route_b_thread = std::thread{[&] {
				uml::routing::ActiveRouteScope route_b{
					&runtime_b, &network_b,
					uml::routing::ActiveRouteScope::DeferredPublication{}};
				route_b.commit();
				const auto bound = uml::routing::current_route();
				{
					std::lock_guard lock{release_mutex};
					b_bound = bound.runtime == &runtime_b &&
						bound.network == &network_b;
					b_committed = true;
				}
				release_changed.notify_all();
				std::unique_lock lock{release_mutex};
				release_changed.wait(lock, [&] { return release_b; });
			}};
		}, "route commit did not reach its deterministic boundary");
	require_route(during_commit, nullptr, nullptr,
		"commit exposed a mixed runtime/network route");
	{
		std::unique_lock lock{release_mutex};
		release_changed.wait(lock, [&] { return b_committed; });
	}
	require(b_bound, "committed thread did not bind both route endpoints");

	const auto during_b_retirement = resolve_during_transaction(
		uml::routing::testing::TransactionBoundary::retirement, [&] {
			{
				std::lock_guard lock{release_mutex};
				release_b = true;
			}
			release_changed.notify_all();
		}, "route retirement did not reach its deterministic boundary");
	route_b_thread.join();
	require_route(during_b_retirement, nullptr, nullptr,
		"overlap retirement restored or mixed the surviving fallback");

	const auto during_a_retirement = resolve_during_transaction(
		uml::routing::testing::TransactionBoundary::retirement,
		[&] { route_a.reset(); },
		"final route retirement did not reach its deterministic boundary");
	require_route(during_a_retirement, nullptr, nullptr,
		"final retirement exposed a partial or active route");

	int runtime_c;
	int network_c;
	uml::routing::ActiveRouteScope route_c{&runtime_c, &network_c};
	uml::routing::Route fresh;
	std::thread fresh_resolver{[&] {
		fresh = uml::routing::current_route();
	}};
	fresh_resolver.join();
	require_route(fresh, &runtime_c, &network_c,
		"fresh cohort did not regain a coherent sole fallback");
}

void test_pending_nested_and_allocation_free_behavior()
{
	int runtime_a;
	int network_a;
	int runtime_b;
	int network_b;
	std::optional<uml::routing::ActiveRouteScope> deferred;
	deferred.emplace(&runtime_b, &network_b,
		uml::routing::ActiveRouteScope::DeferredPublication{});
	{
		uml::routing::ActiveRouteScope outer{&runtime_a, &network_a};
		deferred->commit();
		require_route(uml::routing::current_route(),
			&runtime_b, &network_b,
			"deferred commit did not bind both endpoints");
		deferred.reset();
		require_route(uml::routing::current_route(),
			&runtime_a, &network_a,
			"deferred commit did not capture its commit-time outer route");
	}

	uml::testing::fail_next_allocation();
	{
		uml::routing::ActiveRouteScope route_a{
			&runtime_a, &network_a,
			uml::routing::ActiveRouteScope::DeferredPublication{}};
		route_a.commit();
		require_route(uml::routing::current_route(),
			&runtime_a, &network_a,
			"outer thread route was not bound coherently");
		{
			uml::routing::ActiveRouteScope pending{
				&runtime_b, &network_b,
				uml::routing::ActiveRouteScope::DeferredPublication{}};
			require_route(uml::routing::current_route(),
				&runtime_a, &network_a,
				"pending route changed the bound route");
		}
		{
			uml::routing::ActiveRouteScope nested{&runtime_b, &network_b};
			require_route(uml::routing::current_route(),
				&runtime_b, &network_b,
				"nested route did not bind both endpoints");
		}
		require_route(uml::routing::current_route(),
			&runtime_a, &network_a,
			"nested retirement did not restore the exact outer route");
	}
	require(uml::testing::cancel_pending_allocation_failure(),
		"route reservation, commit, or retirement allocated");
}

} // namespace

int main()
{
	try {
		test_publication_and_retirement_are_one_observable_transaction();
		test_pending_nested_and_allocation_free_behavior();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "run routing test failed: %s\n", error.what());
		return EXIT_FAILURE;
	}
}
