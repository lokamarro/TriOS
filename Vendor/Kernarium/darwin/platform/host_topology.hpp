// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::platform {

/* Matches the host query used by the maintained UML kernel when an explicit
 * ncpus= argument is absent.
 */
[[nodiscard]] unsigned online_processor_count() noexcept;

/* The host's physical cores (`hw.physicalcpu`), at least one; the online
 * count where the system does not say. The applications size their default
 * vCPU count from it (runtime/guest_memory_budget.hpp). */
[[nodiscard]] unsigned physical_processor_count() noexcept;

} // namespace uml::platform
