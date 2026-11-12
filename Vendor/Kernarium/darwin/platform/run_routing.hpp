// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::routing {

/* Runtime and device state remain owned by their respective modules.  The
 * routing layer carries only opaque endpoints so both modules can resolve one
 * coherent run identity without depending on each other. */
struct Route final {
	void *runtime{};
	void *network{};
};

class ActiveRouteScope final {
public:
	struct DeferredPublication final {};

	ActiveRouteScope(void *runtime, void *network) noexcept;
	ActiveRouteScope(void *runtime, void *network,
		DeferredPublication) noexcept;
	~ActiveRouteScope();

	ActiveRouteScope(const ActiveRouteScope &) = delete;
	ActiveRouteScope &operator=(const ActiveRouteScope &) = delete;

	/* Reservation and commit are intrusive and allocation-free. */
	void commit() noexcept;

private:
	friend Route current_route() noexcept;

	Route route_;
	ActiveRouteScope *previous_thread_{};
	bool committed_{};
};

/* A bound public entry thread resolves its exact route. An unbound kernel
 * thread resolves a route only while it is the sole, never-ambiguous active
 * run. Empty endpoints mean that the caller must fail closed. */
[[nodiscard]] Route current_route() noexcept;

} // namespace uml::routing
