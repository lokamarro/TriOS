// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif

#if UML_MEMORY_ALLOCATION_TESTING
namespace uml::memory::testing {

using SoftwareAddressSpaceAcquiredHook = void (*)() noexcept;

[[nodiscard]] std::size_t software_domain_count() noexcept;
[[nodiscard]] std::size_t native_domain_count() noexcept;
void set_software_address_space_acquired_hook(
	SoftwareAddressSpaceAcquiredHook hook) noexcept;

} // namespace uml::memory::testing
#endif
