// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_window_test_support.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <cstddef>
#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace uml::testing {

/* A run maps its kernel window where an embedded kernel's run does; default
 * options give the guest 256 MiB. */
inline constexpr std::size_t run_guest_memory = 256U * 1024U * 1024U;

/* A host page at the first address of the native window that a run takes,
 * where a test kernel maps its guest page (page()) and a test asks retired()
 * afterwards. The process holds its window as one protectionless reservation
 * for as long as it lives, so the slot is a part of that reservation, not an
 * allocation of the test: it starts without a mapping, a run's kernel maps
 * over it, and retirement hands it back to the reservation. */
class RunWindowSlot final {
public:
	RunWindowSlot() : size_{host_page()}
	{
		const auto window = ::uml::emulator::testing::native_kernel_window(
			run_guest_memory);
		require(window.start != 0, "could not derive the run's native window");
		address_ = window.start;
		require(retired(),
			"the run's native window is not a protectionless reservation");
	}

	RunWindowSlot(const RunWindowSlot &) = delete;
	RunWindowSlot &operator=(const RunWindowSlot &) = delete;

	/* The guest page one page into the slot. */
	[[nodiscard]] std::uintptr_t page() const noexcept
	{
		return address_ + DARWIN_UML_GUEST_PAGE_SIZE;
	}

	/* Retirement hands the slot back to the run's window reservation, so it
	 * carries no mapping and is not free. */
	[[nodiscard]] bool retired() const noexcept
	{
		return released_into_native_window(address_, size_);
	}

private:
	std::uintptr_t address_{};
	std::size_t size_{};
};

} // namespace uml::testing
