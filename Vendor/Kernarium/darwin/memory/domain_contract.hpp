// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::memory {

using DomainProvider = bool (*)(std::uint64_t &domain) noexcept;
using DomainRetireObserver = void (*)(std::uint64_t domain) noexcept;

/* Runtime supplies the active public-handle/run identity through this neutral
 * callback. Memory never imports runtime state, so the dependency remains
 * runtime -> memory while kernel-created threads can still resolve the sole
 * active handle. A null provider retains the legacy process domain used by
 * standalone MMU/engine tests. */
void set_domain_provider(DomainProvider provider) noexcept;
[[nodiscard]] bool resolve_current_domain(std::uint64_t &domain) noexcept;
[[nodiscard]] std::uint64_t domain_retirement_sequence() noexcept;
void set_domain_retire_observer(DomainRetireObserver observer) noexcept;
void retire_domain(std::uint64_t domain) noexcept;

} // namespace uml::memory
