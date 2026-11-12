// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/run_routing.hpp"
#include "runtime/context.hpp"

namespace uml::testing {

/* The runtime answers the guest kernel's callbacks (guest entry, the software
 * MMU's domain, the stop and interrupt bridges) only through a run's route.
 * A white-box test that makes those calls without an emulator run declares
 * one of these first in main(): it routes a Context of the test's own for the
 * test's lifetime. The declaring thread is bound to it, and the test's other
 * threads resolve it while it is the only route. */
class RuntimeTestRoute final {
public:
	RuntimeTestRoute() noexcept = default;

	RuntimeTestRoute(const RuntimeTestRoute &) = delete;
	RuntimeTestRoute &operator=(const RuntimeTestRoute &) = delete;

private:
	runtime::Context context_;
	routing::ActiveRouteScope route_{&context_, nullptr};
};

} // namespace uml::testing
